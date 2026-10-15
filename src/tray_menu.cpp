#include "tray_menu.h"
#include "osd_window.h"
#include "alert_window.h"
#include "autostart.h"
#include "ui_theme.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace Tray {

static Autostart::State g_autoState;

static HWND g_hParentAppWnd = NULL;
static HWND g_hAcrylicMenu = NULL;
static HWND g_hAcrylicSubMenu = NULL;
static HHOOK g_hMenuMouseHook = NULL;
static bool g_bModalLoop = false;

static int g_curDpi = 96;
static bool g_curDark = false;
static int g_activeSubId = 0; // 1: mouse rate, 2/5: mouse/keyboard sleep, 3/4: brightness/speed
static int g_mainHover = -1;
static int g_subHover = -1;
// Main values and submenu checks always use the same published snapshot.
static Device::State g_menuMouse;
static Keyboard::State g_menuKeyboard;

// Sleep slider state
static bool g_isDraggingSlider = false;
static int g_sliderVal = 10;
static constexpr UINT_PTR WheelCommitTimer=41;
static const int PRESET_SLEEP[] = { 5, 10, 30 };
static inline int S(int val) {
    return MulDiv(val, g_curDpi, 96);
}

static bool IsSleepSlider(int type) {return type==2 || type==5;}
static int SliderMinimum() {return g_activeSubId==5?1:2;}
static int ClampSlider(int value) {return std::clamp(value,SliderMinimum(),120);}
static int SliderFromRatio(double ratio) {return ClampSlider(SliderMinimum()+int(std::clamp(ratio,0.0,1.0)*(120-SliderMinimum())+0.5));}
static int SettingListHit(int type,int y) {
    int count=type==1?7:(type==3 || type==4)?5:0;
    int index=(y-S(8))/S(28);
    return y>=S(8) && index<count?index:-1;
}


// Windows 11 / 10 Acrylic & Theme Hooks
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
} ACCENT_STATE;

typedef struct _ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
} ACCENT_POLICY;

typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;

typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
typedef BOOL (WINAPI *pfnShouldAppsUseDarkMode)();
typedef BOOL (WINAPI *pfnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *pfnFlushMenuThemes)();

static pfnSetWindowCompositionAttribute fnSetWindowCompositionAttribute = NULL;
static pfnShouldAppsUseDarkMode fnShouldAppsUseDarkMode = NULL;
static pfnAllowDarkModeForWindow fnAllowDarkModeForWindow = NULL;
static pfnFlushMenuThemes fnFlushMenuThemes = NULL;

void InitTheme() {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        fnSetWindowCompositionAttribute = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
    }
    HMODULE hUx = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUx) {
        fnShouldAppsUseDarkMode = (pfnShouldAppsUseDarkMode)GetProcAddress(hUx, MAKEINTRESOURCEA(132));
        fnAllowDarkModeForWindow = (pfnAllowDarkModeForWindow)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
        fnFlushMenuThemes = (pfnFlushMenuThemes)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    }
}

static bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD), type = 0;
        // Check SystemUsesLightTheme first since tray popup menus belong to the taskbar/system area
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (val == 0);
        }
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (val == 0);
        }
        RegCloseKey(hKey);
    }
    if (fnShouldAppsUseDarkMode) return fnShouldAppsUseDarkMode();
    return false;
}

static void ApplyModernWindowStyle(HWND hWnd, bool isDark, int w = 0, int h = 0) {
    if (fnAllowDarkModeForWindow) {
        fnAllowDarkModeForWindow(hWnd, isDark ? TRUE : FALSE);
    }
    BOOL darkVal = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, 20, &darkVal, sizeof(darkVal)); // DWMWA_USE_IMMERSIVE_DARK_MODE

    RECT rc = {0};
    GetWindowRect(hWnd, &rc);
    int width = (w > 0) ? w : (rc.right - rc.left);
    int height = (h > 0) ? h : (rc.bottom - rc.top);
    Ui::ApplyRoundCorners(hWnd, width, height, S(12), Ui::Border);

    if (fnSetWindowCompositionAttribute) {
        ACCENT_POLICY policy = {};
        if (isDark) {
            policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
            policy.AccentFlags = 0; // No DWM system rectangular border (eliminated white streaks)
            policy.GradientColor = 0xCC1A1B20; // AABBGGRR
        } else {
            // In light mode, disable acrylic blur-behind composition.
            // Standard GDI rendering leaves Alpha = 0, causing DWM acrylic composition to render as an opaque white wash.
            // Disabling it restores crisp, solid Win32 GDI rendering with full contrast.
            policy.AccentState = ACCENT_DISABLED;
            policy.AccentFlags = 0;
            policy.GradientColor = 0;
        }
        WINDOWCOMPOSITIONATTRIBDATA data = { 19, &policy, sizeof(policy) };
        fnSetWindowCompositionAttribute(hWnd, &data);
    }
}

static int g_batteryStyle = -1;

int GetBatteryStyle() {
    if (g_batteryStyle == -1) {
        HKEY hKey;
        DWORD val = 0, size = sizeof(DWORD), type = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\rapoo-tray", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExW(hKey, L"DualBatteryStyleV2", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
                g_batteryStyle = val <= 2 ? (int)val : 0;
            }
            RegCloseKey(hKey);
        }
        if (g_batteryStyle == -1) g_batteryStyle = 0;
    }
    return g_batteryStyle;
}

int CycleBatteryStyle() {
    g_batteryStyle = (GetBatteryStyle() + 1) % 3;
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\rapoo-tray", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)g_batteryStyle;
        RegSetValueExW(hKey, L"DualBatteryStyleV2", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
    return g_batteryStyle;
}

bool IsAutoRunEnabled() {
    g_autoState=Autostart::Read();
    return g_autoState.status==Autostart::Status::Registered;
}

void ToggleAutoRun() {
    auto before=Autostart::Read();
    bool enable=before.status!=Autostart::Status::Registered;
    g_autoState=Autostart::Set(enable);
    if(g_autoState.error) {
        wchar_t detail[96];StringCchPrintfW(detail,96,L"启动项操作或回读失败（错误 %lu）",g_autoState.error);
        Osd::Show(L"自启动设置未完成",detail);
    }
}

// --------------------------------------------------------------------------
// Submenu Window (Polling Rate & Sleep Slider)
// --------------------------------------------------------------------------

static void DismissSubMenu() {
    if (g_hAcrylicSubMenu) {
        HWND h = g_hAcrylicSubMenu;
        g_hAcrylicSubMenu = NULL;
        DestroyWindow(h);
    }
    g_activeSubId = 0;
    g_subHover = -1;
    g_isDraggingSlider = false;
}

void DismissAllMenus() {
    if (g_hMenuMouseHook) {
        UnhookWindowsHookEx(g_hMenuMouseHook);
        g_hMenuMouseHook = NULL;
    }
    DismissSubMenu();
    if (g_hAcrylicMenu) {
        HWND h = g_hAcrylicMenu;
        g_hAcrylicMenu = NULL;
        DestroyWindow(h);
    }
    g_bModalLoop = false;
    if (g_hParentAppWnd) {
        PostMessageW(g_hParentAppWnd, WM_NULL, 0, 0);
    }
}

static LRESULT CALLBACK MenuMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || 
            wParam == WM_NCLBUTTONDOWN || wParam == WM_NCRBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN) {
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            POINT pt = p->pt;
            RECT rM = {0}, rS = {0};
            if (g_hAcrylicMenu && IsWindow(g_hAcrylicMenu)) GetWindowRect(g_hAcrylicMenu, &rM);
            if (g_hAcrylicSubMenu && IsWindow(g_hAcrylicSubMenu)) GetWindowRect(g_hAcrylicSubMenu, &rS);
            bool inM = (g_hAcrylicMenu && IsWindow(g_hAcrylicMenu)) && PtInRect(&rM, pt);
            bool inS = (g_hAcrylicSubMenu && IsWindow(g_hAcrylicSubMenu)) && PtInRect(&rS, pt);
            if (!inM && !inS) {
                DismissAllMenus();
            }
        }
    }
    return CallNextHookEx(g_hMenuMouseHook, nCode, wParam, lParam);
}

static void QueueKeyboardSetting(KeyboardProtocol::Setting setting,int value) {
    HWND owner=g_hParentAppWnd;
    DismissAllMenus();
    PostMessageW(owner,KeyboardSettingMessage,static_cast<WPARAM>(setting),value);
}

static void ApplySleepSlider() {
    if(g_activeSubId==5) {
        QueueKeyboardSetting(KeyboardProtocol::Setting::Sleep,g_sliderVal*60);
        return;
    }
    HWND owner=g_hParentAppWnd;int value=g_sliderVal;
    DismissAllMenus();PostMessageW(owner,MouseSettingMessage,1,value);
}

static void DrawSubMenuSurface(HDC memDC,const RECT& rc) {
    COLORREF borderCol = Ui::Border;
    COLORREF hoverCol = Ui::Control;
    COLORREF textCol = Ui::Text;
    COLORREF checkCol = Ui::Accent;
    COLORREF trackBgCol = g_curDark ? RGB(45, 50, 60) : RGB(215, 220, 230);

    Ui::Surface(memDC,rc,g_curDpi);

    HFONT hFontNormal = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT hFontBold = CreateFontW(-S(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT hFontSmall = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    SetBkMode(memDC, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(memDC, hFontNormal);

    if (g_activeSubId == 1 || g_activeSubId==3 || g_activeSubId==4) { // Setting list
        const WCHAR* labels[] = { L"125 Hz", L"250 Hz", L"500 Hz", L"1000 Hz", L"2000 Hz", L"4000 Hz", L"8000 Hz" };
        const int hzVals[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
        int currentHz = g_menuMouse.pollingHz;
        const auto& keyboard=g_menuKeyboard;

        int count = g_activeSubId==1 ? 7 : 5;
        int itemH = S(28);
        int y = S(8);
        for (int i = 0; i < count; i++) {
            RECT rItem = { S(6), y + 1, rc.right - S(6), y + itemH - 1 };
            if (i == g_subHover) {
                Ui::Box(memDC, rItem, hoverCol, hoverCol, S(6));
            }

            SetTextColor(memDC, textCol);
            RECT rText = { S(14), y, rc.right - S(32), y + itemH };
            WCHAR option[24];
            StringCchPrintfW(option,24,L"%d%%",i*25);
            DrawTextW(memDC,g_activeSubId==1?labels[i]:option,-1,&rText,DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (g_activeSubId==1 ? hzVals[i]==currentHz : keyboard.lightingValid && i==(g_activeSubId==3?keyboard.brightness:keyboard.lightSpeed)) {
                SetTextColor(memDC, checkCol);
                RECT rCheck = { rc.right - S(28), y, rc.right - S(10), y + itemH };
                DrawTextW(memDC, L"✓", -1, &rCheck, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            y += itemH;
        }
    } else if (IsSleepSlider(g_activeSubId)) { // Shared mouse/keyboard sleep slider
        // Header Row: Label + Value
        SelectObject(memDC, hFontNormal);
        SetTextColor(memDC, textCol);
        RECT rHeader = { S(16), S(14), rc.right - S(122), S(38) };
        DrawTextW(memDC, g_activeSubId==5?L"键盘休眠":L"鼠标休眠", -1, &rHeader, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, hFontBold);
        SetTextColor(memDC, checkCol);
        WCHAR szVal[32];
        StringCchPrintfW(szVal, ARRAYSIZE(szVal), L"%d 分钟", g_sliderVal);
        RECT rVal = { rc.right - S(116), S(14), rc.right - S(16), S(38) };
        DrawTextW(memDC, szVal, -1, &rVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // Slider Track
        int trackX0 = S(20);
        int trackX1 = rc.right - S(20);
        int trackY = S(58);
        int trackH = S(4);

        double ratio = (double)(g_sliderVal - SliderMinimum()) / (double)(120 - SliderMinimum());
        ratio = std::clamp(ratio, 0.0, 1.0);
        int thumbX = trackX0 + (int)(ratio * (trackX1 - trackX0));

        // Draw anti-aliased track & thumb
        Ui::Box(memDC, RECT{ trackX0, trackY - trackH / 2, trackX1, trackY + trackH / 2 + 1 }, trackBgCol, trackBgCol, S(4));
        if (thumbX > trackX0) {
            Ui::Box(memDC, RECT{ trackX0, trackY - trackH / 2, thumbX, trackY + trackH / 2 + 1 }, checkCol, checkCol, S(4));
        }
        Ui::Circle(memDC, thumbX, trackY, S(8), RGB(255, 255, 255), checkCol, std::max(2, S(2)));

        Ui::Label(memDC,g_activeSubId==5?L"1 分钟":L"2 分钟",RECT{S(20),S(70),S(94),S(91)},11,g_curDpi,Ui::Muted);
        Ui::Label(memDC,L"120 分钟",RECT{rc.right-S(105),S(70),rc.right-S(20),S(91)},11,g_curDpi,Ui::Muted,DT_RIGHT);

        // Preset Chips Row
        SelectObject(memDC, hFontSmall);
        int count = int(std::size(PRESET_SLEEP));
        int chipW = (rc.right - S(40) - (count - 1) * S(6)) / count;
        int curX = S(20);
        int chipY0 = S(96);
        int chipY1 = chipY0 + S(26);

        for (int i = 0; i < count; i++) {
            RECT rChip = { curX, chipY0, curX + chipW, chipY1 };
            bool isCur = (g_sliderVal == PRESET_SLEEP[i]);
            COLORREF chipBg = isCur ? checkCol : (g_curDark ? RGB(36, 40, 50) : RGB(232, 236, 244));
            COLORREF chipBorder = isCur ? checkCol : borderCol;
            Ui::Box(memDC, rChip, chipBg, chipBorder, S(6));

            SetTextColor(memDC, isCur ? (g_curDark ? RGB(10, 12, 16) : RGB(255, 255, 255)) : textCol);
            WCHAR szChip[16];
            StringCchPrintfW(szChip, ARRAYSIZE(szChip), L"%d分", PRESET_SLEEP[i]);
            DrawTextW(memDC, szChip, -1, &rChip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            curX += chipW + S(6);
        }
    }

    SelectObject(memDC, oldFont);
    DeleteObject(hFontNormal);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

}


// Submenu Window Procedure
static LRESULT CALLBACK AcrylicSubWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND hOther = (HWND)lParam;
                if (hOther != g_hAcrylicMenu && hOther != g_hAcrylicSubMenu) {
                    DismissAllMenus();
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEWHEEL: {
            if (IsSleepSlider(g_activeSubId)) { // Sleep Slider
                short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int step = (delta > 0) ? 1 : -1;
                if (GetKeyState(VK_SHIFT) & 0x8000) step *= 5;
                g_sliderVal = ClampSlider(g_sliderVal + step);
                SetTimer(hWnd,WheelCommitTimer,250,nullptr);

                if(IsWindow(hWnd)) InvalidateRect(hWnd, NULL, FALSE);
                if (g_hAcrylicMenu) InvalidateRect(g_hAcrylicMenu, NULL, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            KillTimer(hWnd,WheelCommitTimer);
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);
            RECT rc;
            GetClientRect(hWnd, &rc);

            if (IsSleepSlider(g_activeSubId)) {
                // Sleep Slider Track interaction
                int trackX0 = S(20);
                int trackX1 = rc.right - S(20);
                int trackY = S(58);

                if (my >= trackY - S(14) && my <= trackY + S(14) && mx >= trackX0 - S(8) && mx <= trackX1 + S(8)) {
                    g_isDraggingSlider = true;
                    SetCapture(hWnd);
                    double ratio = (double)(mx - trackX0) / (double)(trackX1 - trackX0);
                    ratio = std::clamp(ratio, 0.0, 1.0);
                    g_sliderVal = SliderFromRatio(ratio);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }

                // Preset Chips interaction (y = S(98), h = S(26))
                int chipY0 = S(96);
                int chipY1 = chipY0 + S(26);
                if (my >= chipY0 && my <= chipY1) {
                    int count = int(std::size(PRESET_SLEEP));
                    int chipW = (rc.right - S(40) - (count - 1) * S(6)) / count;
                    int curX = S(20);
                    for (int i = 0; i < count; i++) {
                        if (mx >= curX && mx <= curX + chipW) {
                            g_sliderVal = PRESET_SLEEP[i];
                            ApplySleepSlider();

                            if(IsWindow(hWnd)) InvalidateRect(hWnd, NULL, FALSE);
                            if (g_hAcrylicMenu) InvalidateRect(g_hAcrylicMenu, NULL, FALSE);
                            return 0;
                        }
                        curX += chipW + S(6);
                    }
                }
            }
            break;
        }

        case WM_MOUSEMOVE: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);
            RECT rc;
            GetClientRect(hWnd, &rc);

            if (IsSleepSlider(g_activeSubId) && g_isDraggingSlider) {
                int trackX0 = S(20);
                int trackX1 = rc.right - S(20);
                double ratio = (double)(mx - trackX0) / (double)(trackX1 - trackX0);
                ratio = std::clamp(ratio, 0.0, 1.0);
                int newVal = SliderFromRatio(ratio);
                if (newVal != g_sliderVal) {
                    g_sliderVal = newVal;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }

            if (g_activeSubId == 1 || g_activeSubId==3 || g_activeSubId==4) { // Setting list
                int newH = SettingListHit(g_activeSubId,my);
                if (newH != g_subHover) {
                    g_subHover = newH;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (IsSleepSlider(g_activeSubId) && g_isDraggingSlider) {
                g_isDraggingSlider = false;
                ReleaseCapture();

                ApplySleepSlider();

                if (g_hAcrylicMenu) InvalidateRect(g_hAcrylicMenu, NULL, FALSE);
                return 0;
            }

            RECT rc{};GetClientRect(hWnd,&rc);
            int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
            g_subHover=x>=0 && x<rc.right?SettingListHit(g_activeSubId,y):-1;
            if((g_activeSubId==3 || g_activeSubId==4) && g_subHover>=0) {
                auto setting=g_activeSubId==3?KeyboardProtocol::Setting::Brightness:KeyboardProtocol::Setting::Speed;
                QueueKeyboardSetting(setting,g_subHover);
                return 0;
            }

            if (g_activeSubId == 1 && g_subHover >= 0) {
                const int hzVals[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
                int hz = hzVals[g_subHover];
                DismissAllMenus();

                PostMessageW(g_hParentAppWnd,MouseSettingMessage,0,hz);
                return 0;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBm = (HBITMAP)SelectObject(memDC, hbm);

            DrawSubMenuSurface(memDC,rc);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBm);
            DeleteObject(hbm);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if(wParam==WheelCommitTimer){KillTimer(hWnd,WheelCommitTimer);ApplySleepSlider();return 0;}
            break;
        case WM_DESTROY:
            KillTimer(hWnd,WheelCommitTimer);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowSubMenuWindow(int subType, RECT rItemScreen) {
    if (g_activeSubId == subType && g_hAcrylicSubMenu) return;
    DismissSubMenu();
    g_activeSubId = subType;

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(g_hAcrylicMenu, GWLP_HINSTANCE);
    static bool s_subRegistered = false;
    if (!s_subRegistered) {
        WNDCLASSEXW wcSub = {0};
        wcSub.cbSize = sizeof(wcSub);
        wcSub.lpfnWndProc = AcrylicSubWndProc;
        wcSub.hInstance = hInst;
        wcSub.lpszClassName = L"RapooModernSubMenuWnd";
        wcSub.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wcSub);
        s_subRegistered = true;
    }

    int subW = IsSleepSlider(subType) ? S(260) : S(140); // 260px wide for sleep slider
    int subH = IsSleepSlider(subType) ? S(138) : subType==1 ? S(212) : S(156);

    HMONITOR hMon = MonitorFromPoint({ rItemScreen.left, rItemScreen.top }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    int subX = rItemScreen.right + S(4);
    if (subX + subW > mi.rcWork.right) {
        subX = rItemScreen.left - subW - S(4);
    }
    int subY = rItemScreen.top - S(4);
    if (subY + subH > mi.rcWork.bottom) {
        subY = mi.rcWork.bottom - subH - S(4);
    }
    if (subY < mi.rcWork.top) subY = mi.rcWork.top;

    if (IsSleepSlider(subType)) {
        g_sliderVal = ClampSlider(subType==5 ? (g_menuKeyboard.sleepSeconds+30)/60 : g_menuMouse.sleepMinutes);
    }

    g_hAcrylicSubMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"RapooModernSubMenuWnd",
        L"",
        WS_POPUP,
        subX, subY, subW, subH,
        g_hAcrylicMenu, NULL, hInst, NULL
    );

    ApplyModernWindowStyle(g_hAcrylicSubMenu, g_curDark, subW, subH);
    ShowWindow(g_hAcrylicSubMenu, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hAcrylicSubMenu);
}

// --------------------------------------------------------------------------
// Main Acrylic Context Menu
// --------------------------------------------------------------------------

struct MenuItemData {
    int id;
    const WCHAR* label;
    WCHAR value[32];
    bool isHeader;
    bool isSeparator;
    bool isInteractive;
    bool isDisabled;
    bool charging;
    int submenuType; // 0: None, 1: Polling, 2: Sleep, 3: Brightness, 4: Effect speed
    bool isModel;
    bool connected;
    bool wired;
};

static std::vector<MenuItemData> g_mainItems;

static int GetItemH(int idx) {
    if (idx < 0 || idx >= (int)g_mainItems.size()) return 0;
    if (g_mainItems[idx].isHeader) return S(40);
    if (g_mainItems[idx].isModel) return S(28);
    if (g_mainItems[idx].isSeparator) return S(9);
    return S(32);
}

static int GetItemY(int targetIdx) {
    int y = S(8);
    for (int i = 0; i < targetIdx; i++) {
        y += GetItemH(i);
    }
    return y;
}

static int HitTestMain(int my) {
    int y = S(8);
    for (size_t i = 0; i < g_mainItems.size(); i++) {
        int h = GetItemH((int)i);
        if (my >= y && my < y + h) {
            if (g_mainItems[i].isHeader || g_mainItems[i].isSeparator || g_mainItems[i].isDisabled) return -1;
            return (int)i;
        }
        y += h;
    }
    return -1;
}

static bool pendingSettings[6]{};
void SetSettingPending(int submenuType,bool pending) {
    if(submenuType>0 && submenuType<6)pendingSettings[submenuType]=pending;
}
static std::vector<MenuItemData> BuildMainItems(const Device::State& st,const Keyboard::State& keyboard,bool autoEnabled) {
    // Populate Menu Items
    std::vector<MenuItemData> items;

    MenuItemData itemHeader = { 0 };
    itemHeader.isHeader = true;
    itemHeader.label = L"鼠标";
    itemHeader.charging=st.isConnected && st.batteryValid && st.isCharging;
    if(st.isConnected && st.batteryValid)StringCchPrintfW(itemHeader.value,32,L"%d%%",st.battery);
    else StringCchCopyW(itemHeader.value,32,L"—");
    items.push_back(itemHeader);

    MenuItemData itemBat = { 0 };
    itemBat.label = L"VT3S MAX 大师版 V2";
    itemBat.isModel=itemBat.isDisabled=true;itemBat.connected=st.isConnected;itemBat.wired=st.isWired;
    items.push_back(itemBat);

    MenuItemData itemDpi = { 0 };
    itemDpi.label = L"DPI";
    itemDpi.isDisabled = !st.isConnected;
    if (st.isConnected) {
        StringCchPrintfW(itemDpi.value, 32, L"%d", st.dpiX);
    } else {
        StringCchCopyW(itemDpi.value, 32, L"—");
    }
    items.push_back(itemDpi);

    MenuItemData itemPoll = { 0 };
    itemPoll.label = L"回报率";
    itemPoll.isInteractive = true;
    itemPoll.isDisabled = !st.isConnected || st.pollingHz<=0;
    itemPoll.submenuType = 1;
    if (st.isConnected && st.pollingHz>0) {
        StringCchPrintfW(itemPoll.value, 32, L"%d Hz", st.pollingHz);
    } else {
        StringCchCopyW(itemPoll.value, 32, L"—");
    }
    items.push_back(itemPoll);

    MenuItemData itemSleep = { 0 };
    itemSleep.label = L"休眠";
    itemSleep.isInteractive = true;
    itemSleep.isDisabled = !st.isConnected || st.sleepMinutes<=0;
    itemSleep.submenuType = 2; // Slider submenu
    if (st.isConnected && st.sleepMinutes>0) {
        StringCchPrintfW(itemSleep.value, 32, L"%d 分钟", st.sleepMinutes);
    } else {
        StringCchCopyW(itemSleep.value, 32, L"—");
    }
    items.push_back(itemSleep);

    MenuItemData sepKeyboard = {0};sepKeyboard.isSeparator=true;items.push_back(sepKeyboard);
    MenuItemData keyboardHeader = {0};keyboardHeader.isHeader=true;keyboardHeader.label=L"键盘";
    keyboardHeader.charging=keyboard.isConnected && keyboard.batteryValid && keyboard.isCharging;
    if(keyboard.isConnected && keyboard.batteryValid)StringCchPrintfW(keyboardHeader.value,32,L"%d%%",keyboard.battery);
    else StringCchCopyW(keyboardHeader.value,32,L"—");items.push_back(keyboardHeader);
    MenuItemData itemKeyboard = {0};
    itemKeyboard.label=L"LEOBOG HI98C Pro";
    itemKeyboard.isModel=itemKeyboard.isDisabled=true;itemKeyboard.connected=keyboard.isConnected;itemKeyboard.wired=keyboard.isWired;
    items.push_back(itemKeyboard);

    for(int type=3;type<=4;++type) {
        MenuItemData light={0};light.label=type==3?L"灯光亮度":L"灯效速度";
        light.isInteractive=true;light.isDisabled=!keyboard.isConnected || !keyboard.lightingValid || !keyboard.lightingWritable || keyboard.lightingStale;light.submenuType=type;
        if(keyboard.isConnected && keyboard.lightingValid && !keyboard.lightingStale) {
            StringCchPrintfW(light.value,32,L"%d%%",(type==3?keyboard.brightness:keyboard.lightSpeed)*25);
        } else StringCchCopyW(light.value,32,L"—");
        items.push_back(light);
    }
    MenuItemData keyboardSleep={0};keyboardSleep.label=L"休眠";keyboardSleep.isInteractive=true;
    keyboardSleep.isDisabled=!keyboard.isConnected || !keyboard.sleepValid || !keyboard.lightingWritable || keyboard.sleepStale;keyboardSleep.submenuType=5;
    if(keyboard.isConnected && keyboard.sleepValid && !keyboard.sleepStale) {
        if(keyboard.sleepSeconds%60==0)StringCchPrintfW(keyboardSleep.value,32,L"%d 分钟",keyboard.sleepSeconds/60);
        else StringCchPrintfW(keyboardSleep.value,32,L"%d 秒",keyboard.sleepSeconds);
    } else StringCchCopyW(keyboardSleep.value,32,L"—");
    items.push_back(keyboardSleep);

    MenuItemData sep1 = { 0 };
    sep1.isSeparator = true;
    items.push_back(sep1);

    MenuItemData itemAlerts = {1003};
    itemAlerts.label=L"低电量提醒设置"; itemAlerts.isInteractive=true;
    items.push_back(itemAlerts);

    MenuItemData itemAuto = { 1001 };
    itemAuto.label = L"开机自启动";
    itemAuto.isInteractive = true;
    StringCchCopyW(itemAuto.value, 32, autoEnabled ? Autostart::Label(g_autoState) :
        g_autoState.status==Autostart::Status::Disabled?L"未开启":Autostart::Label(g_autoState));
    items.push_back(itemAuto);

    MenuItemData sep2 = { 0 };
    sep2.isSeparator = true;
    items.push_back(sep2);

    MenuItemData itemExit = { 1002 };
    itemExit.label = L"退出程序";
    itemExit.isInteractive = true;
    items.push_back(itemExit);

    for(auto& item:items)if(item.submenuType>0 && pendingSettings[item.submenuType])item.isDisabled=true;
    return items;
}

static void SetMenuSnapshot(const Device::State& mouse,const Keyboard::State& keyboard,bool autoEnabled) {
    g_menuMouse=mouse;g_menuKeyboard=keyboard;
    g_mainItems=BuildMainItems(g_menuMouse,g_menuKeyboard,autoEnabled);
}
void UpdateMenuState(const Device::State& mouse,const Keyboard::State& keyboard) {
    if(!g_hAcrylicMenu)return;
    SetMenuSnapshot(mouse,keyboard,IsAutoRunEnabled());
    if(g_mainHover>=0 && g_mainItems[g_mainHover].isDisabled)DismissSubMenu();
    InvalidateRect(g_hAcrylicMenu,nullptr,FALSE);
    if(g_hAcrylicSubMenu)InvalidateRect(g_hAcrylicSubMenu,nullptr,FALSE);
}

static int MenuItemHeight(const MenuItemData& item,int dpi) {
    return Ui::Scale(item.isHeader?40:item.isModel?28:item.isSeparator?9:32,dpi);
}
static void DrawMenuSurface(HDC hdc,const RECT& rc,int dpi,bool dark,const std::vector<MenuItemData>& items,int hover) {
    (void)dark;auto S=[dpi](int n){return Ui::Scale(n,dpi);};
    Ui::Surface(hdc,rc,dpi);int y=S(8);
    for(size_t i=0;i<items.size();++i) {
        const auto& item=items[i];int h=MenuItemHeight(item,dpi);
        if(item.isSeparator) {HPEN p=CreatePen(PS_SOLID,1,Ui::Border);auto old=SelectObject(hdc,p);MoveToEx(hdc,S(14),y+S(4),nullptr);LineTo(hdc,rc.right-S(14),y+S(4));SelectObject(hdc,old);DeleteObject(p);}
        else if(item.isHeader) {
            bool keyboard=wcscmp(item.label,L"键盘")==0;Ui::DeviceIcon(hdc,S(17),y+S(7),dpi,keyboard);
            Ui::Label(hdc,item.label,RECT{S(52),y,rc.right-S(110),y+h},16,dpi,Ui::Text,DT_LEFT,true);
            Ui::Label(hdc,item.value,RECT{rc.right-S(110),y,rc.right-S(item.charging?37:18),y+h},25,dpi,Ui::Accent,DT_RIGHT,true);
            if(item.charging)Ui::ChargeIcon(hdc,rc.right-S(30),y+S(11),dpi);
        } else if(item.isModel) {
            Ui::Label(hdc,item.label,RECT{S(18),y,rc.right-S(95),y+h},12,dpi,Ui::Muted);
            Ui::Connection(hdc,RECT{rc.right-S(82),y+S(2),rc.right-S(18),y+S(26)},dpi,item.connected,item.wired);
        } else {
            if(int(i)==hover && item.isInteractive && !item.isDisabled)Ui::Box(hdc,RECT{S(8),y,rc.right-S(8),y+h},Ui::Control,Ui::Control,S(8));
            bool arrow=item.submenuType>0 && !item.isDisabled;
            Ui::Label(hdc,item.label,RECT{S(18),y,rc.right-S(item.value[0]?134:18),y+h},13,dpi,item.isDisabled?Ui::Muted:Ui::Text);
            Ui::Label(hdc,item.value,RECT{rc.right-S(134),y,rc.right-S(arrow?37:18),y+h},13,dpi,Ui::Muted,DT_RIGHT);
            if(arrow)Ui::Label(hdc,L"›",RECT{rc.right-S(31),y,rc.right-S(14),y+h},18,dpi,Ui::Muted,DT_CENTER);
        }
        y+=h;
    }
}

static LRESULT CALLBACK AcrylicMainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND hOther = (HWND)lParam;
                if (hOther != g_hAcrylicMenu && hOther != g_hAcrylicSubMenu) {
                    DismissAllMenus();
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            int my = HIWORD(lParam);
            int newH = HitTestMain(my);
            if (newH != g_mainHover) {
                g_mainHover = newH;
                InvalidateRect(hWnd, NULL, FALSE);

                if (newH >= 0 && g_mainItems[newH].submenuType > 0 && !g_mainItems[newH].isDisabled) {
                    RECT rcItem = { 0, GetItemY(newH), S(320), GetItemY(newH) + GetItemH(newH) };
                    POINT ptTopLeft = { rcItem.left, rcItem.top };
                    POINT ptBottomRight = { rcItem.right, rcItem.bottom };
                    ClientToScreen(hWnd, &ptTopLeft);
                    ClientToScreen(hWnd, &ptBottomRight);
                    RECT rScreen = { ptTopLeft.x, ptTopLeft.y, ptBottomRight.x, ptBottomRight.y };
                    ShowSubMenuWindow(g_mainItems[newH].submenuType, rScreen);
                } else if (newH >= 0 && g_mainItems[newH].submenuType == 0) {
                    DismissSubMenu();
                }
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            POINT pt;
            GetCursorPos(&pt);
            if (g_hAcrylicSubMenu) {
                RECT rSub;
                GetWindowRect(g_hAcrylicSubMenu, &rSub);
                if (PtInRect(&rSub, pt)) return 0;
            }
            if (g_mainHover != -1) {
                g_mainHover = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int idx = HitTestMain(HIWORD(lParam));
            if (idx >= 0 && g_mainItems[idx].isInteractive && !g_mainItems[idx].isDisabled) {
                int cmd = g_mainItems[idx].id;
                DismissAllMenus();
                if (cmd == 1001) { // Toggle Auto Run
                    ToggleAutoRun();
                } else if (cmd == 1003) {
                    PostMessageW(g_hParentAppWnd,WM_APP+3,0,0);
                } else if (cmd == 1002) { // Exit
                    PostMessageW(g_hParentAppWnd,WM_CLOSE,0,0);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            // Offscreen double-buffering eliminates text/row flicker during hover updates
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBm = (HBITMAP)SelectObject(memDC, hbm);

            DrawMenuSurface(memDC, rc, g_curDpi, g_curDark, g_mainItems, g_mainHover);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBm);
            DeleteObject(hbm);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowMenu(HWND hWndOwner) {
    if (g_bModalLoop) return;
    Device::ForceRefresh();Keyboard::ForceRefresh(); // Show immediately; refresh in the reader, never block the menu.
    g_hParentAppWnd = hWndOwner;
    g_curDark = true;

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    g_curDpi = 0;
    HMODULE hShcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hShcore) {
        typedef HRESULT (WINAPI *pfnGetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
        pfnGetDpiForMonitor fnGetDpiMon = (pfnGetDpiForMonitor)GetProcAddress(hShcore, "GetDpiForMonitor");
        UINT dpiX = 0, dpiY = 0;
        if (fnGetDpiMon && hMon && SUCCEEDED(fnGetDpiMon(hMon, 0, &dpiX, &dpiY)) && dpiX > 0) {
            g_curDpi = (int)dpiX;
        }
        FreeLibrary(hShcore);
    }
    if (g_curDpi <= 0) {
        HMODULE hUser = GetModuleHandleW(L"user32.dll");
        if (hUser) {
            typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
            pfnGetDpiForWindow fnGetDpi = (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
            if (fnGetDpi && hWndOwner) g_curDpi = fnGetDpi(hWndOwner);
        }
    }
    if (g_curDpi <= 0) g_curDpi = 96;

    Device::State st = Device::GetCurrentState();

    SetMenuSnapshot(st,Keyboard::GetCurrentState(),IsAutoRunEnabled());

    int totalH = S(16);
    for (size_t i = 0; i < g_mainItems.size(); i++) {
        totalH += GetItemH((int)i);
    }
    int menuW = S(320);

    int posX = pt.x - S(10);
    int posY = pt.y - totalH - S(10);
    if (posX + menuW > mi.rcWork.right) posX = mi.rcWork.right - menuW - S(8);
    if (posX < mi.rcWork.left) posX = mi.rcWork.left + S(8);
    if (posY < mi.rcWork.top) posY = pt.y + S(10);
    if (posY + totalH > mi.rcWork.bottom) posY = mi.rcWork.bottom - totalH - S(8);

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hWndOwner, GWLP_HINSTANCE);
    static bool s_mainRegistered = false;
    if (!s_mainRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AcrylicMainWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"RapooModernMainMenuWnd";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
        s_mainRegistered = true;
    }

    g_mainHover = -1;
    g_activeSubId = 0;

    g_hAcrylicMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"RapooModernMainMenuWnd",
        L"",
        WS_POPUP,
        posX, posY, menuW, totalH,
        hWndOwner, NULL, hInst, NULL
    );

    ApplyModernWindowStyle(g_hAcrylicMenu, g_curDark, menuW, totalH);
    SetForegroundWindow(g_hAcrylicMenu);
    ShowWindow(g_hAcrylicMenu, SW_SHOW);
    UpdateWindow(g_hAcrylicMenu);

    g_hMenuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MenuMouseHookProc, hInst, 0);

    g_bModalLoop = true;
    MSG msg{};
    while (g_bModalLoop && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DismissAllMenus();
            break;
        }
        if (Alert::HandleSettingsMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_bModalLoop) {
        DismissAllMenus();
    }
    if (msg.message == WM_QUIT) {
        PostQuitMessage((int)msg.wParam);
    }
}

// --------------------------------------------------------------------------
// Taskbar Battery Icon & Tooltip
// --------------------------------------------------------------------------

HICON CreateBatteryIcon(int battery, bool isCharging, bool isConnected, int size, bool isDark, int keyboardBattery, bool keyboardCharging, bool keyboardValid, int styleOverride) {
    size = std::clamp(size,16,64);
    const int SS = 4;
    int W = size * SS;
    int H = size * SS;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvPixels = NULL;
    HBITMAP hbmColor = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvPixels, NULL, 0);
    if (!hbmColor || !pvPixels) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }

    uint32_t* hi = (uint32_t*)pvPixels;
    std::memset(hi, 0, W * H * 4);

    auto HiPixel = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < W && y >= 0 && y < H) {
            hi[y * W + x] = c;
        }
    };

    auto FillRoundRect = [&](int x0, int y0, int x1, int y1, uint32_t col, int rad) {
        if (rad > (x1 - x0) / 2) rad = (x1 - x0) / 2;
        if (rad > (y1 - y0) / 2) rad = (y1 - y0) / 2;
        if (rad < 0) rad = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                int dx = (x < x0 + rad) ? (x0 + rad - x) : (x > x1 - rad) ? (x - (x1 - rad)) : 0;
                int dy = (y < y0 + rad) ? (y0 + rad - y) : (y > y1 - rad) ? (y - (y1 - rad)) : 0;
                if (dx * dx + dy * dy <= rad * rad) HiPixel(x, y, col);
            }
        }
    };

    auto DrawDigitScaled = [&](const uint16_t* rows, int fw, int fh,
                               int x0, int y0, int scale, int bold_w, uint32_t c_digit) {
        for (int r = 0; r < fh; ++r) {
            for (int c = 0; c < fw; ++c) {
                if (!((rows[r] >> (fw - 1 - c)) & 1)) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale + bold_w; ++dx) {
                        int px = x0 + c * scale + dx;
                        int py = y0 + r * scale + dy;
                        HiPixel(px, py, c_digit);
                    }
                }
            }
        }
    };

    auto L2S = [&](float v) {
        return (int)(v * (float)(size * SS) / 16.0f + 0.5f);
    };

    int style = styleOverride>=0 ? std::clamp(styleOverride,0,2) : GetBatteryStyle();

    // All styles show mouse above keyboard. Blue = mouse; violet = keyboard.
    static const uint16_t digits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}
    };
    for(int row=0;row<2;++row) {
        int percent=std::clamp(row?keyboardBattery:battery,0,100);
        bool valid=row?keyboardValid:isConnected, charging=row?keyboardCharging:isCharging;
        uint32_t color=!valid?0xFF94A3B8:charging?(isDark?0xFF22C55E:0xFF15803D):percent<=Alert::GetConfig(row).warning?(isDark?0xFFEF4444:0xFFB91C1C):row?(isDark?0xFFC4A5FF:0xFF6D28D9):(isDark?0xFF38BDF8:0xFF0369A1);
        int top=L2S(row?9:1);
        if(style==0 || style==2) {
            wchar_t text[4]; if(valid) StringCchPrintfW(text,4,L"%d",percent); else StringCchCopyW(text,4,L"--");
            int pixelScale=style==0?std::max(1,size/12):std::max(1,size/16);
            pixelScale=std::min(pixelScale,std::max(1,(size/2-2)/5));
            int scale=pixelScale*SS, count=int(wcslen(text)), start=((W-(count*4-1)*scale)/2/SS)*SS;
            if(style==0)top=(row?size/2+1:1)*SS;
            for(int i=0;i<count;++i) {
                if(text[i]=='-') FillRoundRect(start+i*4*scale,top+2*scale,start+i*4*scale+2*scale,top+3*scale-1,color,0);
                else DrawDigitScaled(digits[text[i]-L'0'],3,5,start+i*4*scale,top,scale,0,color);
            }
            // Hybrid: a one-pixel progress bar under each number.
            if(style==2) {
                FillRoundRect(L2S(1),top+L2S(6),L2S(15)-1,top+L2S(7)-1,isDark?0xFF475569:0xFFCBD5E1,0);
                int width=L2S(14)*percent/100;
                if(valid && width>0) FillRoundRect(L2S(1),top+L2S(6),L2S(1)+width-1,top+L2S(7)-1,color,0);
            }
        } else {
            if(!valid) {
                for(int x=L2S(1);x<L2S(15);x+=L2S(4)) FillRoundRect(x,top+L2S(2),x+L2S(2),top+L2S(3),color,0);
                continue;
            }
            FillRoundRect(L2S(0),top,L2S(15),top+L2S(5),isDark?0xFFD7E0EC:0xFF334155,L2S(1));
            FillRoundRect(L2S(1),top+L2S(1),L2S(14),top+L2S(4),isDark?0xFF18202B:0xFFF0F3F7,0);
            int width=L2S(13)*percent/100;
            if(width>0) FillRoundRect(L2S(1),top+L2S(1),L2S(1)+width-1,top+L2S(4),color,0);
        }
    }

    // Downsample SSxSS to size x size with smooth box filtering
    BITMAPINFO bomi = {0};
    bomi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bomi.bmiHeader.biWidth = size;
    bomi.bmiHeader.biHeight = -size;
    bomi.bmiHeader.biPlanes = 1;
    bomi.bmiHeader.biBitCount = 32;
    bomi.bmiHeader.biCompression = BI_RGB;

    void* pvOut = NULL;
    HBITMAP hbmOut = CreateDIBSection(hdcMem, &bomi, DIB_RGB_COLORS, &pvOut, NULL, 0);
    if (!hbmOut || !pvOut) {
        DeleteObject(hbmColor);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }

    uint32_t* out = (uint32_t*)pvOut;
    const int SS2 = SS * SS;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t sumA = 0, sumR = 0, sumG = 0, sumB = 0;
            for (int sy = 0; sy < SS; ++sy) {
                const uint32_t* row = hi + (y * SS + sy) * W + x * SS;
                for (int sx = 0; sx < SS; ++sx) {
                    uint32_t c = row[sx];
                    uint32_t a = (c >> 24) & 0xFF;
                    if (a > 0) {
                        sumA += a;
                        sumR += (c >> 16) & 0xFF;
                        sumG += (c >>  8) & 0xFF;
                        sumB += (c      ) & 0xFF;
                    }
                }
            }
            uint8_t avgA = (uint8_t)(sumA / SS2);
            if (avgA == 0) {
                out[y * size + x] = 0;
            } else {
                uint8_t avgR = (uint8_t)(sumR / SS2);
                uint8_t avgG = (uint8_t)(sumG / SS2);
                uint8_t avgB = (uint8_t)(sumB / SS2);
                uint8_t prR = (uint8_t)((avgR * avgA) / 255);
                uint8_t prG = (uint8_t)((avgG * avgA) / 255);
                uint8_t prB = (uint8_t)((avgB * avgA) / 255);
                out[y * size + x] = ((uint32_t)avgA << 24) | ((uint32_t)prR << 16) | ((uint32_t)prG << 8) | (uint32_t)prB;
            }
        }
    }

    int maskPitch = ((size + 15) / 16) * 2;
    std::vector<BYTE> maskBits(maskPitch * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t px = out[y * size + x];
            uint8_t a = (px >> 24) & 0xFF;
            if (a < 32) {
                maskBits[y * maskPitch + (x / 8)] |= (1 << (7 - (x % 8)));
            }
        }
    }
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmOut;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmOut);
    DeleteObject(hbmMask);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    return hIcon;
}

void UpdateTooltip(NOTIFYICONDATAW& nid, const Device::State& state, const Keyboard::State& keyboard) {
    wchar_t m[64],k[64];
    StringCchPrintfW(nid.szTip,ARRAYSIZE(nid.szTip),L"%s: %s\n%s: %s\n鼠标 DPI %d · %d Hz",
        state.modelName,Battery::Description(state.isConnected,state.batteryValid,state.battery,state.isCharging,state.isWired,m,64),
        keyboard.modelName,Battery::Description(keyboard.isConnected,keyboard.batteryValid,keyboard.battery,keyboard.isCharging,keyboard.isWired,k,64),state.dpiX,state.pollingHz);
}

SIZE DrawMenuPreview(HDC dc,int dpi,bool dark,const Device::State& mouse,const Keyboard::State& keyboard) {
    auto items=BuildMainItems(mouse,keyboard,false);
    int height=MulDiv(16,dpi,96);
    for(const auto& item:items)height+=MenuItemHeight(item,dpi);
    RECT rect{0,0,MulDiv(320,dpi,96),height};
    DrawMenuSurface(dc,rect,dpi,dark,items,-1);return {rect.right,rect.bottom};
}

} // namespace Tray
