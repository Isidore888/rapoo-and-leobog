#include "alert_window.h"
#include "ui_theme.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>
#include <deque>
#include <string>

namespace Alert {

static HWND g_hAlertWnd = NULL;
static bool s_isShown = false;
static bool s_isCritical = false;
static int  s_battery = 0;
static bool s_isWired = false;
static WCHAR s_modelName[64] = {0};

static bool s_btnHover = false;
static bool s_closeHover = false;
static bool s_trackingMouse = false;

static AlertPolicy::Config configs[2];
static AlertPolicy::Tracker trackers[2];
struct Pending {
    int device; std::wstring name; int battery; bool critical, wired;
    bool consumedWarning=false,consumedCritical=false;
};
static std::deque<Pending> pending;
static int activeDevice = -1;
static void ShowNext() {
    if(!g_hAlertWnd || s_isShown || pending.empty()) return;
    auto next = pending.front(); pending.pop_front(); activeDevice = next.device;
    Show(next.name.c_str(),next.battery,next.critical,next.wired);
}
static void CancelDevice(int id) {
    // Queueing reserves a notification; only a displayed card consumes it.
    for(const auto& p:pending) if(p.device==id) {
        if(p.consumedWarning)trackers[id].warned=false;
        if(p.consumedCritical)trackers[id].critical=false;
    }
    pending.erase(std::remove_if(pending.begin(),pending.end(),[id](const Pending& p){return p.device==id;}),pending.end());
    // Advance only after both devices have been checked: the next card may
    // itself have recovered since it was queued.
    if(activeDevice==id) {
        if(g_hAlertWnd) ShowWindow(g_hAlertWnd,SW_HIDE);
        s_isShown=false;activeDevice=-1;
    }
}
static void LoadConfig() {
    HKEY key;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\rapoo-tray",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return;
    DWORD data[7]{};DWORD size=sizeof(data),type=0;
    if(RegQueryValueExW(key,L"DualBatteryAlerts",nullptr,&type,(BYTE*)data,&size)==ERROR_SUCCESS && type==REG_BINARY && size==sizeof(data) && data[0]==1) {
        AlertPolicy::Config candidate[2];
        for(int i=0;i<2;++i)candidate[i]={data[1+i*3]!=0,int(data[2+i*3]),int(data[3+i*3])};
        if(AlertPolicy::Valid(candidate[0]) && AlertPolicy::Valid(candidate[1]))for(int i=0;i<2;++i)configs[i]=candidate[i];
    }
    RegCloseKey(key);
}
AlertPolicy::Config GetConfig(int id) { return configs[id==1?1:0]; }
bool SaveConfig(const AlertPolicy::Config (&config)[2]) {
    if(!AlertPolicy::Valid(config[0])||!AlertPolicy::Valid(config[1])) return false;
    DWORD data[7]={1};for(int i=0;i<2;++i){data[1+i*3]=config[i].enabled;data[2+i*3]=config[i].warning;data[3+i*3]=config[i].critical;}
    HKEY key;if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\rapoo-tray",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    LONG result=RegSetValueExW(key,L"DualBatteryAlerts",0,REG_BINARY,(BYTE*)data,sizeof(data));RegCloseKey(key);
    if(result!=ERROR_SUCCESS)return false;
    for(int i=0;i<2;++i){
        bool thresholdsChanged = configs[i].warning != config[i].warning || configs[i].critical != config[i].critical;
        bool changed = thresholdsChanged || configs[i].enabled != config[i].enabled;
        configs[i]=config[i];
        if(thresholdsChanged) trackers[i]={};
        if(changed) CancelDevice(i);
    }
    ShowNext();
    return true;
}

static inline int ScaleDpi(int val, int dpi) {
    return MulDiv(val, dpi, 96);
}

static UINT GetCurrentWindowDpi(HWND hWnd) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
        pfnGetDpiForWindow fnGetDpi = (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
        if (fnGetDpi && hWnd) {
            UINT d = fnGetDpi(hWnd);
            if (d > 0) return d;
        }
    }
    return 96;
}

static bool IsSystemDark() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD), type = 0;
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (val == 0);
        }
        RegCloseKey(hKey);
    }
    return true;
}

static void CalculateAlertPosition(int targetW, int targetH, int dpi, int* outX, int* outY) {
    auto S = [dpi](int v) { return ScaleDpi(v, dpi); };

    // 1. Dynamically locate the Windows taskbar
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    APPBARDATA abd = { sizeof(APPBARDATA) };
    abd.hWnd = hTaskbar;
    BOOL hasAppBar = (hTaskbar && SHAppBarMessage(ABM_GETTASKBARPOS, &abd));

    // 2. Identify the monitor where the taskbar is located
    HMONITOR hMon = NULL;
    if (hTaskbar && IsWindow(hTaskbar)) {
        hMon = MonitorFromWindow(hTaskbar, MONITOR_DEFAULTTOPRIMARY);
    } else {
        POINT pt = { 0, 0 };
        GetCursorPos(&pt);
        hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(hMon, &mi)) {
        RECT rcWork = { 0 };
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
        *outX = rcWork.right - targetW - S(10);
        *outY = rcWork.bottom - targetH - S(8);
        return;
    }

    RECT rcWork = mi.rcWork;
    RECT rcMon  = mi.rcMonitor;

    int marginSide = S(10); // Gap from screen right edge
    int marginTask = S(8);  // Snug gap hugging the taskbar edge

    if (hasAppBar) {
        switch (abd.uEdge) {
            case ABE_BOTTOM: {
                // Taskbar is at bottom: abd.rc.top is the dynamic top edge
                *outX = rcWork.right - targetW - marginSide;
                *outY = abd.rc.top - targetH - marginTask;
                break;
            }
            case ABE_TOP: {
                // Taskbar is at top: abd.rc.bottom is the dynamic bottom edge
                *outX = rcWork.right - targetW - marginSide;
                *outY = abd.rc.bottom + marginTask;
                break;
            }
            case ABE_LEFT: {
                // Taskbar is at left: abd.rc.right is the dynamic right edge
                *outX = abd.rc.right + marginTask;
                *outY = rcWork.bottom - targetH - marginSide;
                break;
            }
            case ABE_RIGHT: {
                // Taskbar is at right: abd.rc.left is the dynamic left edge
                *outX = abd.rc.left - targetW - marginTask;
                *outY = rcWork.bottom - targetH - marginSide;
                break;
            }
            default: {
                *outX = rcWork.right - targetW - marginSide;
                *outY = rcWork.bottom - targetH - marginTask;
                break;
            }
        }
    } else {
        *outX = rcWork.right - targetW - marginSide;
        *outY = rcWork.bottom - targetH - marginTask;
    }

    // Safety boundary clamping
    if (*outX + targetW > rcMon.right)  *outX = rcMon.right - targetW;
    if (*outX < rcMon.left)             *outX = rcMon.left;
    if (*outY + targetH > rcMon.bottom) *outY = rcMon.bottom - targetH;
    if (*outY < rcMon.top)              *outY = rcMon.top;
}

static void GetButtonRects(HWND hWnd, RECT* pClose, RECT* pConfirm) {
    UINT dpi = GetCurrentWindowDpi(hWnd);
    auto S = [dpi](int v) { return ScaleDpi(v, dpi); };

    RECT rc;
    GetClientRect(hWnd, &rc);

    if (pClose) {
        pClose->left   = rc.right - S(34);
        pClose->top    = S(10);
        pClose->right  = rc.right - S(10);
        pClose->bottom = S(34);
    }

    if (pConfirm) {
        pConfirm->left   = rc.right - S(108);
        pConfirm->top    = rc.bottom - S(38);
        pConfirm->right  = rc.right - S(16);
        pConfirm->bottom = rc.bottom - S(12);
    }
}

void DrawAlert(HDC dc,int dpi,const wchar_t* model,int battery,bool critical,bool buttonHover,bool closeHover) {
    auto S=[dpi](int n){return Ui::Scale(n,dpi);};RECT rc{0,0,S(390),S(150)};Ui::Surface(dc,rc,dpi);
    COLORREF warning=critical?RGB(255,160,164):RGB(231,182,121);
    Ui::Box(dc,RECT{S(14),S(14),S(119),S(136)},Ui::Control,Ui::Control,S(20));
    wchar_t value[16];StringCchPrintfW(value,16,L"%d%%",battery);Ui::Label(dc,value,RECT{S(17),S(40),S(116),S(101)},30,dpi,warning,DT_CENTER,true);
    bool keyboard=model && wcsstr(model,L"LEOBOG");
    Ui::Label(dc,keyboard?(critical?L"键盘电量很低":L"键盘电量偏低"):(critical?L"鼠标电量很低":L"鼠标电量偏低"),RECT{S(135),S(17),S(354),S(48)},17,dpi,Ui::Text,DT_LEFT,true);
    Ui::Label(dc,critical?L"请及时充电":L"建议充电",RECT{S(136),S(53),S(365),S(78)},13,dpi,Ui::Muted);
    Ui::Box(dc,RECT{S(282),S(112),S(374),S(138)},buttonHover?Ui::Accent:Ui::Control,Ui::Border,S(8));
    Ui::Label(dc,L"我知道了",RECT{S(282),S(112),S(374),S(138)},12,dpi,buttonHover?Ui::Background:Ui::Text,DT_CENTER);
    Ui::Label(dc,L"×",RECT{S(356),S(10),S(380),S(34)},17,dpi,closeHover?Ui::Text:Ui::Muted,DT_CENTER);
}

static LRESULT CALLBACK AlertWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SETCURSOR: {
            RECT rClose, rConfirm;
            GetButtonRects(hWnd, &rClose, &rConfirm);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            if (PtInRect(&rClose, pt) || PtInRect(&rConfirm, pt)) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        case WM_MOUSEMOVE: {
            if (!s_trackingMouse) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&tme);
                s_trackingMouse = true;
            }

            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rClose, rConfirm;
            GetButtonRects(hWnd, &rClose, &rConfirm);

            bool newClose = PtInRect(&rClose, pt);
            bool newBtn   = PtInRect(&rConfirm, pt);

            if (newClose != s_closeHover || newBtn != s_btnHover) {
                s_closeHover = newClose;
                s_btnHover   = newBtn;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            s_trackingMouse = false;
            s_closeHover    = false;
            s_btnHover      = false;
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rClose, rConfirm;
            GetButtonRects(hWnd, &rClose, &rConfirm);

            if (PtInRect(&rClose, pt) || PtInRect(&rConfirm, pt)) {
                Hide();
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;HDC dc=BeginPaint(hWnd,&ps);RECT rc;GetClientRect(hWnd,&rc);DrawAlert(dc,GetCurrentWindowDpi(hWnd),s_modelName,s_battery,s_isCritical,s_btnHover,s_closeHover);EndPaint(hWnd,&ps);return 0;
        }

        case WM_DESTROY:
            g_hAlertWnd = NULL;
            s_isShown = false;
            return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void Init(HINSTANCE hInstance) {
    LoadConfig();
    WNDCLASSEXW wc = { 0 };
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = AlertWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"RapooAlertCardWnd";

    RegisterClassExW(&wc);

    g_hAlertWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"RapooAlertCardWnd",
        L"RapooBatteryAlert",
        WS_POPUP,
        0, 0, 10, 10,
        NULL, NULL, hInstance, NULL
    );

    if (g_hAlertWnd) {
        // Windows 11 hardware-antialiased rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33)
        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(g_hAlertWnd, 33, &corner, sizeof(corner));

        BOOL isDark = TRUE;
        DwmSetWindowAttribute(g_hAlertWnd, 20, &isDark, sizeof(isDark));
    }
}

void Cleanup() {
    CloseSettings();
    if (g_hAlertWnd && IsWindow(g_hAlertWnd)) {
        DestroyWindow(g_hAlertWnd);
        g_hAlertWnd = NULL;
    }
    s_isShown = false;
    activeDevice=-1;pending.clear();
    for(auto& tracker:trackers)tracker={};
}

void Show(const WCHAR* modelName, int battery, bool isCritical, bool isWired) {
    s_isCritical = isCritical;
    s_battery    = battery;
    s_isWired    = isWired;
    s_btnHover   = false;
    s_closeHover = false;
    if (modelName) {
        StringCchCopyW(s_modelName, ARRAYSIZE(s_modelName), modelName);
    } else {
        StringCchCopyW(s_modelName, ARRAYSIZE(s_modelName), L"通用");
    }

    if (!g_hAlertWnd) return;
    SetWindowTextW(g_hAlertWnd,s_modelName);

    // Dynamically update DWM border color (Warning: Amber / Critical: Red)
    COLORREF borderCol = s_isCritical ? RGB(239, 68, 68) : RGB(245, 158, 11);
    DwmSetWindowAttribute(g_hAlertWnd, 34, &borderCol, sizeof(borderCol));

    BOOL isDark = TRUE;
    DwmSetWindowAttribute(g_hAlertWnd, 20, &isDark, sizeof(isDark));

    UINT dpi = GetCurrentWindowDpi(g_hAlertWnd);
    auto S = [dpi](int v) { return ScaleDpi(v, dpi); };

    int targetW = S(390);
    int targetH = S(150);

    // Calculate dynamic position hugging the taskbar
    int x = 0, y = 0;
    CalculateAlertPosition(targetW, targetH, dpi, &x, &y);

    // NOTE: SetWindowRgn is removed!
    // SetWindowRgn disables DWM anti-aliased corner rounding and drop shadow,
    // causing 1-bit jagged staircase pixels (毛边).
    // DWM with DWMWCP_ROUND automatically provides GPU subpixel antialiasing and drop shadow.

    SetWindowPos(g_hAlertWnd, HWND_TOPMOST, x, y, targetW, targetH, SWP_NOACTIVATE);
    InvalidateRect(g_hAlertWnd, NULL, TRUE);
    UpdateWindow(g_hAlertWnd);
    ShowWindow(g_hAlertWnd, SW_SHOWNOACTIVATE);

    s_isShown = true;
}

void Hide() {
    if (g_hAlertWnd) {
        ShowWindow(g_hAlertWnd, SW_HIDE);
    }
    s_isShown = false;
    activeDevice = -1;
    ShowNext();
}

static void CheckOne(int id, const wchar_t* name, bool connected, bool fresh, int battery, bool charging, bool wired) {
    const auto previous=trackers[id];
    int level=trackers[id].Evaluate(configs[id],connected,fresh,battery,charging);
    // Evaluate may rearm and warn in the same call at the 100% boundary.
    // The event, rather than the previous cycle's flags, identifies its reservation.
    const bool reserveWarning=level==1 || (level==2 && !previous.warned);
    const bool reserveCritical=level==2;
    if (!configs[id].enabled || !connected || !fresh || charging || battery<0 || battery>configs[id].warning) {
        CancelDevice(id);return;
    }
    const bool critical=battery<=configs[id].critical;
    if(activeDevice==id && s_isShown) {
        // Update the current card in place, including an immediate escalation.
        if(s_battery!=battery || s_isCritical!=critical || s_isWired!=wired)
            Show(name,battery,critical,wired);
        return;
    }
    for(auto& item:pending) if(item.device==id) {
        if(level) {
            item.consumedWarning=item.consumedWarning || reserveWarning;
            item.consumedCritical=item.consumedCritical || reserveCritical;
        }
        if(item.critical && !critical && item.consumedCritical) {
            trackers[id].critical=false;item.consumedCritical=false;
            // A warning already displayed earlier in this cycle must not be
            // displayed again merely because its queued escalation recovered.
            if(!item.consumedWarning){CancelDevice(id);return;}
        }
        item.name=name;item.battery=battery;item.critical=critical;item.wired=wired;
        return;
    }
    if(level){
        pending.erase(std::remove_if(pending.begin(),pending.end(),[id](const Pending& p){return p.device==id;}),pending.end());
        pending.push_back({id,name,battery,critical,wired,
            reserveWarning,reserveCritical});
    }
}
void CheckBattery(const Device::State& mouse, const Keyboard::State& keyboard) {
    CheckOne(0,mouse.modelName,mouse.isConnected,mouse.batteryValid,mouse.battery,mouse.isCharging,mouse.isWired);
    CheckOne(1,keyboard.modelName,keyboard.isConnected,keyboard.batteryValid,keyboard.battery,keyboard.chargingKnown && keyboard.isCharging,keyboard.isWired);
    ShowNext();
}

} // namespace Alert
