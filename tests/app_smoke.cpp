#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <vector>
#include <stdexcept>
static DWORD appPid = 0;
static const wchar_t* wantedClass = nullptr;
static HWND foundWindow = nullptr;
static BOOL CALLBACK FindOwned(HWND window, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    wchar_t cls[128]{}; GetClassNameW(window, cls, 128);
    if (pid == appPid && wcscmp(cls, wantedClass) == 0) { foundWindow = window; return FALSE; }
    return TRUE;
}
static HWND Find(const wchar_t* cls) { wantedClass = cls; foundWindow = nullptr; EnumWindows(FindOwned, 0); return foundWindow; }
static HWND WaitWindow(const wchar_t* cls){HWND window=nullptr;for(int i=0;i<40&&!window;++i){Sleep(50);window=Find(cls);}return window;}
static void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct CursorBackup{POINT saved{};CursorBackup(){GetCursorPos(&saved);}~CursorBackup(){SetCursorPos(saved.x,saved.y);}};
struct StyleBackup {
    bool existed=false;DWORD type=0;std::vector<BYTE> bytes;
    StyleBackup(){HKEY key;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\rapoo-tray",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return;DWORD n=0;if(RegQueryValueExW(key,L"DualBatteryStyleV2",nullptr,&type,nullptr,&n)==ERROR_SUCCESS){bytes.resize(n);existed=RegQueryValueExW(key,L"DualBatteryStyleV2",nullptr,&type,bytes.data(),&n)==ERROR_SUCCESS;}RegCloseKey(key);}
    ~StyleBackup(){HKEY key;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\rapoo-tray",0,KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return;if(existed)RegSetValueExW(key,L"DualBatteryStyleV2",0,type,bytes.data(),DWORD(bytes.size()));else RegDeleteValueW(key,L"DualBatteryStyleV2");RegCloseKey(key);}
};
static void Capture(HWND window, const wchar_t* path) {
    RECT r{}; Require(GetWindowRect(window, &r), "GetWindowRect"); int w = r.right-r.left, h = r.bottom-r.top;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen); HBITMAP bmp = CreateCompatibleBitmap(screen,w,h);
    auto old = SelectObject(dc,bmp); Require(PrintWindow(window,dc,0), "PrintWindow");
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    std::vector<BYTE> pixels(w*h*4); SelectObject(dc,old); Require(GetDIBits(dc,bmp,0,h,pixels.data(),&info,DIB_RGB_COLORS)!=0,"GetDIBits");
    BITMAPFILEHEADER file{}; file.bfType=0x4D42; file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader); file.bfSize=file.bfOffBits+DWORD(pixels.size());
    FILE* out=_wfopen(path,L"wb"); Require(out!=nullptr,"capture file");
    fwrite(&file,sizeof(file),1,out); fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out); fwrite(pixels.data(),pixels.size(),1,out); fclose(out);
    DeleteObject(bmp); DeleteDC(dc); ReleaseDC(nullptr,screen);
}
int main() {
    StyleBackup style;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{}; wchar_t command[]=L"bin\\rapoo-tray.exe";
    if (!CreateProcessW(nullptr,command,nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) { puts("FAIL: launch"); return 1; }
    appPid=process.dwProcessId; CloseHandle(process.hThread); int result=0;
    try {
        HWND main=nullptr;
        for(int i=0;i<50&&!main;++i){Sleep(100); main=Find(L"RapooTrayMessageWnd");}
        Require(main!=nullptr,"owned main window absent (check another instance)"); Sleep(800);
        Require(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT,"app exited early");
        PostMessageW(main,WM_APP+1,0,WM_LBUTTONUP); Sleep(900);
        HWND osd=Find(L"RapooOsdPopupWnd"); Require(osd&&IsWindowVisible(osd),"status popup absent"); Capture(osd,L"verification/app-status.bmp");
        wchar_t title[64]{};GetWindowTextW(osd,title,64);Require(wcscmp(title,L"鼠标和键盘电量")==0,"single click did not show dual card");
        Sleep(4700);Require(!IsWindowVisible(osd),"status popup did not expire");
        for(int i=0;i<3;++i){PostMessageW(main,WM_APP+2,1,1);Sleep(100);Require(!IsWindowVisible(osd),"connection update caused an unsolicited popup");}
        PostMessageW(main,WM_APP+1,0,WM_LBUTTONUP);PostMessageW(main,WM_APP+1,0,WM_LBUTTONDBLCLK);PostMessageW(main,WM_APP+1,0,WM_LBUTTONUP);Sleep(900);
        GetWindowTextW(osd,title,64);Require(wcscmp(title,L"RapooOSD")==0,"double click also invoked single click");
        {
            CursorBackup cursor;NOTIFYICONIDENTIFIER id{sizeof(id)};id.hWnd=main;id.uID=1;RECT icon{};
            Require(SUCCEEDED(Shell_NotifyIconGetRect(&id,&icon)),"tray icon rectangle absent");
            Require(SetCursorPos((icon.left+icon.right)/2,(icon.top+icon.bottom)/2),"hover position");
            PostMessageW(main,WM_APP+1,0,WM_MOUSEMOVE);Sleep(800);
            GetWindowTextW(osd,title,64);Require(IsWindowVisible(osd)&&wcscmp(title,L"鼠标和键盘电量")==0,"hover card absent");Capture(osd,L"verification/render/app-hover.bmp");
            POINT point{};GetCursorPos(&point);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
            SetCursorPos(monitor.rcWork.left+10,monitor.rcWork.top+10);Sleep(300);Require(!IsWindowVisible(osd),"hover card remained after leaving icon");
        }
        PostMessageW(main,WM_APP+1,0,WM_RBUTTONUP); Sleep(300);
        HWND menu=WaitWindow(L"RapooModernMainMenuWnd"); Require(menu&&IsWindowVisible(menu),"context menu absent"); Capture(menu,L"verification/app-menu.bmp");
        UINT dpi=GetDpiForWindow(menu); if(!dpi)dpi=96;
        // Header, five device rows and separator precede the alert settings row.
        int settingsY=MulDiv(8,dpi,96)+MulDiv(52,dpi,96)+5*MulDiv(32,dpi,96)+MulDiv(9,dpi,96)+MulDiv(16,dpi,96);
        PostMessageW(menu,WM_LBUTTONUP,0,MAKELPARAM(MulDiv(100,dpi,96),settingsY)); Sleep(300);
        HWND settings=Find(L"DualBatteryAlertSettings"); Require(settings&&IsWindowVisible(settings),"main settings absent");
        Capture(settings,L"verification/app-settings.bmp"); PostMessageW(settings,WM_COMMAND,IDCANCEL,0); Sleep(100); Require(!Find(L"DualBatteryAlertSettings"),"cancel failed");
        PostMessageW(main,RegisterWindowMessageW(L"TaskbarCreated"),0,0); Sleep(300);
        Require(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT,"restart notification crashed app");
        PostMessageW(main,WM_APP+1,0,WM_RBUTTONUP); menu=WaitWindow(L"RapooModernMainMenuWnd"); Require(menu!=nullptr,"exit menu absent");
        RECT client{}; GetClientRect(menu,&client);
        PostMessageW(menu,WM_LBUTTONUP,0,MAKELPARAM(MulDiv(100,dpi,96),client.bottom-MulDiv(24,dpi,96)));
        Require(WaitForSingleObject(process.hProcess,10000)==WAIT_OBJECT_0,"menu shutdown timed out");
        DWORD code=1; GetExitCodeProcess(process.hProcess,&code); Require(code==0,"app exit code");
        puts("PASS: main launch, tray status/context menu, menu settings/cancel, taskbar notification handling and menu exit");
    } catch(const std::exception& e) {
        fprintf(stderr,"FAIL: %s\n",e.what()); result=1;
        if(HWND main=Find(L"RapooTrayMessageWnd"))PostMessageW(main,WM_CLOSE,0,0);
        if(WaitForSingleObject(process.hProcess,10000)!=WAIT_OBJECT_0)fprintf(stderr,"Owned test app still running, PID=%lu\n",appPid);
    }
    CloseHandle(process.hProcess); return result;
}
