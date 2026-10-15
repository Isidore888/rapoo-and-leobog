#include <windows.h>
#include <shellapi.h>
#include "../src/tray_menu.h"
#include "../src/alert_window.h"
#include "../src/diagnostics.h"
#include <cassert>
#include <cstdio>

static int icons=0,notifications=0,warnings=0,checks=0,repaints=0,style=0,iconSize=16;
static bool shellWorks=true,iconWorks=true;
static Device::State mouse;
static Keyboard::State keyboard;
static AlertPolicy::Config configs[2];
static BOOL WINAPI TestShell(DWORD action,PNOTIFYICONDATAW){if(action==NIM_MODIFY)++notifications;return shellWorks;}
static int WINAPI TestWarning(HWND,LPCWSTR,LPCWSTR,UINT){++warnings;return IDOK;}
static LSTATUS WINAPI TestTheme(HKEY,LPCWSTR,DWORD,REGSAM,PHKEY){return ERROR_FILE_NOT_FOUND;}
static int WINAPI TestMetrics(int metric){return metric==SM_CXSMICON?iconSize: ::GetSystemMetrics(metric);}
namespace Device {State TestState(){return mouse;}}
namespace Keyboard {State TestState(){return keyboard;}}
namespace Tray {
HICON TestIcon(int,bool,bool,int,bool,int,bool,bool,int){++icons;return iconWorks?CopyIcon(LoadIconW(nullptr,IDI_APPLICATION)):nullptr;}
int TestStyle(){return style;}
}
namespace Alert {
void TestCheck(const Device::State&,const Keyboard::State&){++checks;}
AlertPolicy::Config TestConfig(int device){return configs[device];}
}
#define GetCurrentState TestState
#define CreateBatteryIcon TestIcon
#define GetBatteryStyle TestStyle
#define CheckBattery TestCheck
#define GetConfig TestConfig
#define Shell_NotifyIconW TestShell
#define MessageBoxW TestWarning
#define RegOpenKeyExW TestTheme
#define GetSystemMetrics TestMetrics
#include "../src/main.cpp"
#undef GetSystemMetrics
#undef RegOpenKeyExW
#undef MessageBoxW
#undef Shell_NotifyIconW
#undef CheckBattery
#undef GetConfig
#undef GetBatteryStyle
#undef CreateBatteryIcon
#undef GetCurrentState
static BOOL WINAPI TestInvalidate(HWND,LPCRECT,BOOL){++repaints;return TRUE;}
#define InvalidateRect TestInvalidate
#define GetConfig TestConfig
#include "../src/osd_window.cpp"
#undef GetConfig
#undef InvalidateRect

int main() {
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"refresh test",WS_POPUP,
        GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)+10000,10000,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(window);g_hMainWnd=window;
    mouse.isConnected=mouse.batteryValid=true;mouse.battery=80;mouse.pollingHz=2000;
    keyboard.isConnected=keyboard.batteryValid=true;keyboard.battery=90;
    RefreshTrayUI(mouse);assert(icons==1 && notifications==1);
    for(int i=0;i<100;++i){++mouse.lastBatteryUpdate;++keyboard.lastBatteryUpdate;RefreshTrayUI(mouse);}
    assert(icons==1 && notifications==1 && checks==101);
    mouse.dpiX=1600;RefreshTrayUI(mouse);assert(icons==1 && notifications==2);
    keyboard.isWired=true;RefreshTrayUI(mouse);assert(icons==1 && notifications==3);
    mouse.battery=79;RefreshTrayUI(mouse);assert(icons==2);
    keyboard.isCharging=true;RefreshTrayUI(mouse);assert(icons==3);
    mouse.batteryValid=false;RefreshTrayUI(mouse);assert(icons==4);
    ++style;RefreshTrayUI(mouse);assert(icons==5);
    iconSize=24;RefreshTrayUI(mouse);assert(icons==6);
    g_uTaskbarRestartMsg=RegisterWindowMessageW(L"refresh test taskbar");
    MainWndProc(window,g_uTaskbarRestartMsg,0,0);assert(icons==7);
    shellWorks=false;mouse.battery=78;RefreshTrayUI(mouse);int drawn=icons,sent=notifications;
    RefreshTrayUI(mouse);assert(icons==drawn && notifications==sent+1);
    shellWorks=true;RefreshTrayUI(mouse);sent=notifications;RefreshTrayUI(mouse);assert(notifications==sent);
    iconWorks=false;mouse.battery=77;RefreshTrayUI(mouse);drawn=icons;
    iconWorks=true;RefreshTrayUI(mouse);assert(icons==drawn+1);
    drawn=icons;++configs[0].warning;RefreshTrayUI(mouse);assert(icons==drawn+1);
    drawn=icons;++configs[1].warning;RefreshTrayUI(mouse);assert(icons==drawn+1);
    Diagnostics::writeFailure=ERROR_DISK_FULL;RefreshTrayUI(mouse);assert(warnings==1);
    RefreshTrayUI(mouse);assert(warnings==1);
    Diagnostics::writeFailure=ERROR_WRITE_FAULT;RefreshTrayUI(mouse);assert(warnings==1);
    Diagnostics::TakeWriteFailure();

    Osd::g_hOsdWnd=window;Osd::statusMode=true;ShowWindow(window,SW_SHOWNOACTIVATE);
    Osd::UpdateStatus(mouse,keyboard);int painted=repaints;
    for(int i=0;i<100;++i){++mouse.lastBatteryUpdate;++keyboard.lastLightingUpdate;++keyboard.lightingRevision;Osd::UpdateStatus(mouse,keyboard);}
    assert(repaints==painted);
    auto changed=[&]{Osd::UpdateStatus(mouse,keyboard);assert(repaints==++painted);};
    keyboard.lightingValid=true;keyboard.brightness=4;changed();
    keyboard.brightness=3;changed();keyboard.lightSpeed=2;changed();keyboard.lightingStale=true;changed();
    keyboard.sleepValid=true;keyboard.sleepSeconds=300;changed();keyboard.sleepStale=true;changed();
    keyboard.batteryValid=false;changed();mouse.dpiX=800;changed();mouse.pollingHz=1000;changed();
    mouse.sleepMinutes=7;changed();keyboard.isConnected=false;changed();
    ++configs[0].warning;changed();++configs[1].warning;changed();
    ShowWindow(window,SW_HIDE);++mouse.battery;Osd::UpdateStatus(mouse,keyboard);assert(repaints==painted);
    DestroyIcon(g_hCurrentTrayIcon);g_hCurrentTrayIcon=nullptr;Osd::g_hOsdWnd=nullptr;g_hMainWnd=nullptr;DestroyWindow(window);
    puts("PASS: unchanged tray/status skips100 redraws; tooltip, style, DPI, taskbar recreation, expiry, settings, threshold changes and failures still update; one warning, no real tray/HID/registry writes");
}
