#include "osd_window.h"
#include "alert_window.h"
#include "ui_theme.h"
#include <strsafe.h>
#include <algorithm>

namespace Osd {

static HWND g_hOsdWnd = NULL;
static WCHAR g_textLine1[128] = {0};
static WCHAR g_textLine2[128] = {0};
static WCHAR g_textLine3[128] = {0};
static BYTE g_osdAlpha = 0;
static bool statusMode=false, statusDark=true,valueMode=false,valueKeyboard=false;
static wchar_t valueLabel[64]{},valueText[32]{};
static Device::State statusMouse;
static Keyboard::State statusKeyboard;

static const COLORREF OSD_KEY_COLOR = RGB(10, 12, 16);
static const UINT_PTR TIMER_OSD_HIDE = 101;
static const UINT_PTR TIMER_OSD_FADE = 102;

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

static LRESULT CALLBACK OsdWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            UINT dpi = GetCurrentWindowDpi(hWnd);
            auto S = [dpi](int v) { return ScaleDpi(v, dpi); };

            if(statusMode) {
                DrawStatus(memDC,dpi,statusDark,statusMouse,statusKeyboard);
                BitBlt(hdc,0,0,rc.right,rc.bottom,memDC,0,0,SRCCOPY);
                SelectObject(memDC,oldBmp); DeleteObject(memBmp); DeleteDC(memDC);
                EndPaint(hWnd,&ps); return 0;
            }

            if(valueMode) {
                DrawValue(memDC,dpi,valueLabel,valueText,valueKeyboard);BitBlt(hdc,0,0,rc.right,rc.bottom,memDC,0,0,SRCCOPY);
                SelectObject(memDC,oldBmp);DeleteObject(memBmp);DeleteDC(memDC);EndPaint(hWnd,&ps);return 0;
            }

            // Fill with Key Color for transparency
            HBRUSH hKey = CreateSolidBrush(OSD_KEY_COLOR);
            FillRect(memDC, &rc, hKey);
            DeleteObject(hKey);

            RECT rcBox = rc;
            InflateRect(&rcBox, -S(2), -S(2));

            // Modern frosted card background & border
            HBRUSH hBg = CreateSolidBrush(Ui::Background);
            HPEN hBorder = CreatePen(PS_SOLID, 1, Ui::Border);
            HGDIOBJ oldBr = SelectObject(memDC, hBg);
            HGDIOBJ oldPen = SelectObject(memDC, hBorder);

            RoundRect(memDC, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, S(16), S(16));

            SetBkMode(memDC, TRANSPARENT);

            bool hasLine2 = (g_textLine2[0] != 0);
            bool hasLine3 = (g_textLine3[0] != 0);

            // Font 1: Line 1 (Large bold title)
            HFONT hFontBig = CreateFontW(
                -S(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
            );
            HGDIOBJ oldFont = SelectObject(memDC, hFontBig);
            SetTextColor(memDC, RGB(255, 255, 255));

            RECT rcTop = rcBox;
            rcTop.top = rcBox.top + S(8);
            rcTop.bottom = rcTop.top + S(26);
            DrawTextW(memDC, g_textLine1, -1, &rcTop, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Font 2: Line 2 (Sub text)
            HFONT hFontMid = CreateFontW(
                -S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
            );
            SelectObject(memDC, hFontMid);
            SetTextColor(memDC, Ui::Muted);

            RECT rcMid = rcBox;
            rcMid.top = rcTop.bottom + S(2);
            rcMid.bottom = rcMid.top + S(22);
            DrawTextW(memDC, g_textLine2, -1, &rcMid, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Font 3: Line 3 (Status bar / Cyan highlight)
            HFONT hFontSub = NULL;
            if (hasLine3) {
                hFontSub = CreateFontW(
                    -S(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
                );
                SelectObject(memDC, hFontSub);
                SetTextColor(memDC, RGB(120, 210, 255));

                RECT rcBot = rcBox;
                rcBot.top = (hasLine2 ? rcMid.bottom : rcTop.bottom) + S(2);
                rcBot.bottom = rcBot.top + S(22);
                DrawTextW(memDC, g_textLine3, -1, &rcBot, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBr);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            DeleteObject(hFontBig);
            DeleteObject(hFontMid);
            if (hFontSub) DeleteObject(hFontSub);
            DeleteObject(hBorder);
            DeleteObject(hBg);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_OSD_HIDE) {
                KillTimer(hWnd, TIMER_OSD_HIDE);
                SetTimer(hWnd, TIMER_OSD_FADE, 16, NULL);
            } else if (wParam == TIMER_OSD_FADE) {
                if (g_osdAlpha > 15) {
                    g_osdAlpha -= 15;
                    SetLayeredWindowAttributes(hWnd, OSD_KEY_COLOR, g_osdAlpha, LWA_COLORKEY | LWA_ALPHA);
                } else {
                    KillTimer(hWnd, TIMER_OSD_FADE);
                    ShowWindow(hWnd, SW_HIDE);
                    g_osdAlpha = 0;
                }
            }
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

bool Init(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OsdWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RapooOsdPopupWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hOsdWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"RapooOSD",
        WS_POPUP,
        0, 0, 300, 96,
        NULL, NULL, hInstance, NULL
    );

    return (g_hOsdWnd != NULL);
}

void Cleanup() {
    if (g_hOsdWnd && IsWindow(g_hOsdWnd)) {
        DestroyWindow(g_hOsdWnd);
        g_hOsdWnd = NULL;
    }
}

HWND GetWindowHandle() {
    return g_hOsdWnd;
}

void Show(const WCHAR* line1, const WCHAR* line2, const WCHAR* line3, UINT durationMs) {
    if (!g_hOsdWnd) return;
    statusMode=false;valueMode=false;
    SetWindowRgn(g_hOsdWnd,nullptr,TRUE);
    SetWindowTextW(g_hOsdWnd,L"RapooOSD");

    StringCchCopyW(g_textLine1, ARRAYSIZE(g_textLine1), line1 ? line1 : L"");
    StringCchCopyW(g_textLine2, ARRAYSIZE(g_textLine2), line2 ? line2 : L"");
    if (line3 && line3[0]) {
        StringCchCopyW(g_textLine3, ARRAYSIZE(g_textLine3), line3);
    } else {
        g_textLine3[0] = 0;
    }

    // Determine target monitor based on cursor position
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    RECT rcWork = mi.rcWork;

    UINT dpi = GetCurrentWindowDpi(g_hOsdWnd);
    auto S = [dpi](int v) { return ScaleDpi(v, dpi); };

    // Dynamic width & height calculation via DT_CALCRECT
    HDC screenDC = GetDC(NULL);
    HFONT hFontBig = CreateFontW(
        -S(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
    );
    HFONT hFontMid = CreateFontW(
        -S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
    );
    HFONT hFontSub = CreateFontW(
        -S(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
    );

    RECT r1 = {0, 0, 0, 0};
    RECT r2 = {0, 0, 0, 0};
    RECT r3 = {0, 0, 0, 0};

    HGDIOBJ oldF = SelectObject(screenDC, hFontBig);
    DrawTextW(screenDC, g_textLine1, -1, &r1, DT_CALCRECT | DT_SINGLELINE);

    SelectObject(screenDC, hFontMid);
    DrawTextW(screenDC, g_textLine2, -1, &r2, DT_CALCRECT | DT_SINGLELINE);

    int maxTextW = std::max(r1.right - r1.left, r2.right - r2.left);

    bool hasLine3 = (g_textLine3[0] != 0);
    if (hasLine3) {
        SelectObject(screenDC, hFontSub);
        DrawTextW(screenDC, g_textLine3, -1, &r3, DT_CALCRECT | DT_SINGLELINE);
        maxTextW = std::max(maxTextW, (int)(r3.right - r3.left));
    }

    SelectObject(screenDC, oldF);
    DeleteObject(hFontBig);
    DeleteObject(hFontMid);
    DeleteObject(hFontSub);
    ReleaseDC(NULL, screenDC);

    // Add safe horizontal padding (18px each side)
    int targetW = maxTextW + S(36);
    targetW = std::max(targetW, S(210));
    targetW = std::min(targetW, std::min(S(640), int(rcWork.right-rcWork.left)-S(24)));

    int targetH = S(44 + (g_textLine2[0] ? 22 : 0) + (hasLine3 ? 22 : 0));

    int x = rcWork.left + ((rcWork.right - rcWork.left) - targetW) / 2;
    int y = rcWork.bottom - targetH - S(70);

    KillTimer(g_hOsdWnd, TIMER_OSD_HIDE);
    KillTimer(g_hOsdWnd, TIMER_OSD_FADE);

    g_osdAlpha = 240;
    SetLayeredWindowAttributes(g_hOsdWnd, OSD_KEY_COLOR, g_osdAlpha, LWA_COLORKEY | LWA_ALPHA);

    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, targetW, targetH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hOsdWnd, NULL, TRUE);

    SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, durationMs, NULL);
}

void DrawValue(HDC dc,int dpi,const wchar_t* label,const wchar_t* value,bool keyboard) {
    auto S=[dpi](int n){return Ui::Scale(n,dpi);};RECT all{0,0,S(360),S(80)};Ui::Surface(dc,all,dpi);
    Ui::DeviceIcon(dc,S(19),S(27),dpi,keyboard);
    Ui::Label(dc,label,RECT{S(57),S(14),S(192),S(66)},17,dpi,Ui::Muted);
    Ui::Label(dc,value,RECT{S(196),S(10),S(339),S(70)},30,dpi,Ui::Accent,DT_RIGHT,true);
}
void ShowValue(const wchar_t* label,const wchar_t* value,bool keyboard,UINT durationMs) {
    wchar_t combined[128];StringCchPrintfW(combined,128,L"%s %s",label,value);Show(combined,nullptr,nullptr,durationMs);
    if(!g_hOsdWnd)return;valueMode=true;valueKeyboard=keyboard;StringCchCopyW(valueLabel,64,label);StringCchCopyW(valueText,32,value);
    RECT old{};GetWindowRect(g_hOsdWnd,&old);int dpi=GetCurrentWindowDpi(g_hOsdWnd),w=Ui::Scale(360,dpi),h=Ui::Scale(80,dpi);
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(g_hOsdWnd,MONITOR_DEFAULTTONEAREST),&mi);
    int x=std::clamp(int(old.left+(old.right-old.left-w)/2),int(mi.rcWork.left),std::max(int(mi.rcWork.left),int(mi.rcWork.right)-w));
    SetWindowPos(g_hOsdWnd,nullptr,x,old.bottom-h,w,h,SWP_NOACTIVATE|SWP_NOZORDER);SetWindowRgn(g_hOsdWnd,CreateRoundRectRgn(0,0,w+1,h+1,Ui::Scale(22,dpi),Ui::Scale(22,dpi)),TRUE);InvalidateRect(g_hOsdWnd,nullptr,FALSE);
}
void ShowDpiUpdate(int level,int dpix,int dpiy,int battery,int pollingHz,bool isCharging,bool isWired,const WCHAR* modelName) {
    (void)level;(void)dpiy;(void)battery;(void)pollingHz;(void)isCharging;(void)isWired;(void)modelName;
    wchar_t value[32];StringCchPrintfW(value,32,L"%d",dpix);ShowValue(L"DPI",value,false);
}
void DrawStatus(HDC dc,int dpi,bool dark,const Device::State& mouse,const Keyboard::State& keyboard) {
    (void)dark;auto S=[dpi](int n){return Ui::Scale(n,dpi);};Ui::Surface(dc,RECT{0,0,S(360),S(298)},dpi);
    for(int k=0;k<2;++k) {
        bool connected=k?keyboard.isConnected:mouse.isConnected,valid=connected && (k?keyboard.batteryValid:mouse.batteryValid);
        bool wired=k?keyboard.isWired:mouse.isWired,charging=valid && (k?keyboard.isCharging:mouse.isCharging);
        int percent=k?keyboard.battery:mouse.battery;valid=valid && percent>=0 && percent<=100;
        int cx=S(90+k*180),cy=S(79),radius=S(50);COLORREF color=charging?RGB(148,229,181):valid && percent<=Alert::GetConfig(k).warning?RGB(255,184,138):k?Ui::Keyboard:Ui::Accent;
        HPEN p=CreatePen(PS_SOLID,S(5),Ui::Border);auto op=SelectObject(dc,p);auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));Ellipse(dc,cx-radius,cy-radius,cx+radius,cy+radius);
        SelectObject(dc,op);DeleteObject(p);
        if(valid && percent>0) {p=CreatePen(PS_SOLID,S(5),color);SelectObject(dc,p);POINT points[101];int count=std::max(1,percent);
            for(int n=0;n<=count;++n){double angle=-1.5707963267948966+6.283185307179586*n/100;points[n]={cx+LONG(radius*std::cos(angle)),cy+LONG(radius*std::sin(angle))};}Polyline(dc,points,count+1);SelectObject(dc,op);DeleteObject(p);}
        SelectObject(dc,ob);
        wchar_t value[32];if(valid)StringCchPrintfW(value,32,L"%d%%",percent);else StringCchCopyW(value,32,L"—");
        Ui::Label(dc,value,RECT{cx-S(48),cy-S(28),cx+S(48),cy+S(9)},25,dpi,valid?color:Ui::Muted,DT_CENTER,true);
        Ui::Label(dc,k?L"键盘":L"鼠标",RECT{cx-S(45),cy+S(9),cx+S(45),cy+S(35)},13,dpi,Ui::Muted,DT_CENTER);
        if(charging)Ui::ChargeIcon(dc,cx+S(27),cy+S(12),dpi);
        Ui::Connection(dc,RECT{cx-S(34),S(147),cx+S(34),S(173)},dpi,connected,wired);
        for(int row=0;row<3;++row) {
            const wchar_t* label=k?(row==0?L"亮度":row==1?L"速度":L"休眠"):(row==0?L"DPI":row==1?L"回报率":L"休眠");StringCchCopyW(value,32,L"—");
            if(k) {
                if(row<2 && connected && keyboard.lightingValid && !keyboard.lightingStale)StringCchPrintfW(value,32,L"%d%%",(row?keyboard.lightSpeed:keyboard.brightness)*25);
                if(row==2 && connected && keyboard.sleepValid && !keyboard.sleepStale)StringCchPrintfW(value,32,keyboard.sleepSeconds%60?L"%d 秒":L"%d 分钟",keyboard.sleepSeconds%60?keyboard.sleepSeconds:keyboard.sleepSeconds/60);
            } else if(connected) {
                if(row==0 && mouse.dpiX>0)StringCchPrintfW(value,32,L"%d",mouse.dpiX);
                if(row==1 && mouse.pollingHz>0)StringCchPrintfW(value,32,L"%d Hz",mouse.pollingHz);
                if(row==2 && mouse.sleepMinutes>0)StringCchPrintfW(value,32,L"%d 分钟",mouse.sleepMinutes);
            }
            int top=S(192+row*29);Ui::Label(dc,label,RECT{cx-S(66),top,cx-S(8),top+S(27)},12,dpi,Ui::Muted);
            Ui::Label(dc,value,RECT{cx-S(3),top,cx+S(66),top+S(27)},13,dpi,Ui::Text,DT_RIGHT);
        }
    }
}
void ShowStatus(const Device::State& mouse,const Keyboard::State& keyboard,POINT anchor,UINT durationMs) {
    if(!g_hOsdWnd)return;
    statusMode=true;valueMode=false;statusMouse=mouse;statusKeyboard=keyboard;
    statusDark=true;
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&mi);
    // Move first so per-monitor DPI is read from the target monitor.
    SetWindowPos(g_hOsdWnd,nullptr,anchor.x,anchor.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    int dpi=GetCurrentWindowDpi(g_hOsdWnd),w=MulDiv(360,dpi,96),h=MulDiv(298,dpi,96);
    int x=std::clamp(int(anchor.x)-w/2,int(mi.rcWork.left),std::max(int(mi.rcWork.left),int(mi.rcWork.right)-w));
    int y=std::clamp(int(anchor.y)-h-MulDiv(12,dpi,96),int(mi.rcWork.top),std::max(int(mi.rcWork.top),int(mi.rcWork.bottom)-h));
    KillTimer(g_hOsdWnd,TIMER_OSD_HIDE);KillTimer(g_hOsdWnd,TIMER_OSD_FADE);
    g_osdAlpha=255;SetLayeredWindowAttributes(g_hOsdWnd,0,255,LWA_ALPHA);
    SetWindowPos(g_hOsdWnd,HWND_TOPMOST,x,y,w,h,SWP_SHOWWINDOW|SWP_NOACTIVATE);
    HRGN region=CreateRoundRectRgn(0,0,w+1,h+1,MulDiv(14,dpi,96),MulDiv(14,dpi,96));SetWindowRgn(g_hOsdWnd,region,TRUE);
    SetWindowTextW(g_hOsdWnd,L"鼠标和键盘电量");InvalidateRect(g_hOsdWnd,nullptr,TRUE);
    if(durationMs)SetTimer(g_hOsdWnd,TIMER_OSD_HIDE,durationMs,nullptr);
}
void UpdateStatus(const Device::State& mouse,const Keyboard::State& keyboard) {
    if(!IsStatusVisible())return;statusMouse=mouse;statusKeyboard=keyboard;InvalidateRect(g_hOsdWnd,nullptr,FALSE);
}
bool IsStatusVisible(){return statusMode && g_hOsdWnd && IsWindowVisible(g_hOsdWnd);}
void HideStatus(){if(!statusMode)return;KillTimer(g_hOsdWnd,TIMER_OSD_HIDE);KillTimer(g_hOsdWnd,TIMER_OSD_FADE);ShowWindow(g_hOsdWnd,SW_HIDE);}

} // namespace Osd
