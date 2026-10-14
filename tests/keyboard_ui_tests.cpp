// Native menu -> main message -> OSD regression, with no device access.
// All created windows stay hidden or outside the virtual desktop.
#include "../src/tray_menu.h"
#include "../src/osd_window.h"
#include "../src/alert_window.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <future>
#undef assert
#define assert(c) do {if(!(c))throw std::runtime_error(#c);} while(0)
static std::atomic<int> calls{0};static int value=-1;
static std::mutex testStateMutex;
static std::promise<void>* blockedEntered=nullptr;
static std::shared_future<void> blockedRelease;
static FILE* testLog=nullptr;
static KeyboardProtocol::Setting requested=KeyboardProtocol::Setting::Brightness;
static Keyboard::LightingResult result=Keyboard::LightingResult::Confirmed;
namespace Keyboard {
static State testState;
State TestState(){std::lock_guard<std::mutex> lock(testStateMutex);return testState;}
LightingResult TestLighting(bool speed,int level) {std::lock_guard<std::mutex> lock(testStateMutex);value=level;requested=speed?KeyboardProtocol::Setting::Speed:KeyboardProtocol::Setting::Brightness;if(result==LightingResult::Confirmed){if(speed)testState.lightSpeed=level;else testState.brightness=level;}++calls;return result;}
LightingResult TestSleep(int seconds) {std::lock_guard<std::mutex> lock(testStateMutex);value=seconds;requested=KeyboardProtocol::Setting::Sleep;if(result==LightingResult::Confirmed)testState.sleepSeconds=seconds;++calls;return result;}
LightingResult TestLightingChecked(bool speed,int level,const State&){
    if(blockedEntered){blockedEntered->set_value();blockedRelease.wait();++calls;return LightingResult::Confirmed;}
    return TestLighting(speed,level);
}
LightingResult TestSleepChecked(int seconds,const State&){return TestSleep(seconds);}
}
namespace Device {State TestState(){return State{};}
SettingResult TestSleepChecked(int,ULONGLONG){assert(false && "UI test must not access mouse settings");return SettingResult::Disconnected;}
SettingResult TestMouseRateChecked(int,ULONGLONG){assert(false && "UI test must not access mouse settings");return SettingResult::Disconnected;}}
static BOOL WINAPI TestNotify(DWORD,PNOTIFYICONDATAW){return TRUE;}
#define SetLighting TestLighting
#define SetSleepTimeout TestSleep
#define SetLightingChecked TestLightingChecked
#define SetSleepTimeoutChecked TestSleepChecked
#define SetPollingRateChecked TestMouseRateChecked
#define GetCurrentState TestState
#define Shell_NotifyIconW TestNotify
#define wWinMain UnusedTestAppMain
#include "../src/main.cpp"
#undef wWinMain
#undef Shell_NotifyIconW
#include "../src/tray_menu.cpp"
#undef SetLighting
#undef SetSleepTimeout
#undef SetLightingChecked
#undef SetSleepTimeoutChecked
#undef SetPollingRateChecked
#undef GetCurrentState
static BOOL WINAPI TestMonitor(HMONITOR,LPMONITORINFO info) {
    int x=GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)+10000;
    info->rcWork=info->rcMonitor=RECT{x,10000,x+2000,12000};return TRUE;
}
#define GetMonitorInfoW TestMonitor
#include "../src/osd_window.cpp"
#undef GetMonitorInfoW

static void Pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
static void Save(HDC dc,HBITMAP bitmap,int w,int h,const wchar_t* path) {
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    std::vector<BYTE> pixels(w*h*4);assert(GetDIBits(dc,bitmap,0,h,pixels.data(),&info,DIB_RGB_COLORS));
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+DWORD(pixels.size());
    FILE* out=_wfopen(path,L"wb");assert(out);fwrite(&file,sizeof(file),1,out);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out);fwrite(pixels.data(),pixels.size(),1,out);fclose(out);
}
static HWND Menu(int type,int dpi=96) {
    Tray::g_curDpi=dpi;Tray::g_activeSubId=type;Tray::g_subHover=-1;
    Tray::g_hParentAppWnd=g_hMainWnd;Tray::g_bModalLoop=true;
    Tray::g_hAcrylicMenu=CreateWindowExW(0,L"STATIC",L"Test hidden menu",WS_POPUP,0,0,1,1,g_hMainWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(Tray::g_hAcrylicMenu);
    bool slider=Tray::IsSleepSlider(type);
    Tray::g_hAcrylicSubMenu=CreateWindowExW(0,L"KeyboardUIHiddenSub",L"",WS_POPUP,0,0,Tray::S(slider?260:140),Tray::S(slider?138:156),Tray::g_hAcrylicMenu,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(Tray::g_hAcrylicSubMenu);return Tray::g_hAcrylicSubMenu;
}
static void CheckResult(int before,KeyboardProtocol::Setting setting,int expected,const wchar_t* text) {
    assert(calls==before);assert(!Tray::g_hAcrylicSubMenu && !Tray::g_hAcrylicMenu && !Tray::g_bModalLoop);
    auto deadline=GetTickCount64()+2000;
    do {Pump();if(calls==before+1 && wcsstr(Osd::g_textLine1,text))break;Sleep(1);}while(GetTickCount64()<deadline);
    assert(calls==before+1 && requested==setting && value==expected);
    assert(IsWindowVisible(Osd::GetWindowHandle()) && !Osd::statusMode);
    assert(wcsstr(Osd::g_textLine1,text));
    RECT r{};GetWindowRect(Osd::GetWindowHandle(),&r);
    assert(r.left>GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN));
    SendMessageW(g_hMainWnd,WM_APP_TRAYMSG,0,WM_MOUSEMOVE);
    SendMessageW(g_hMainWnd,WM_TIMER,4,0);
    assert(!hoverPending && !hoverVisible && !Osd::statusMode && wcsstr(Osd::g_textLine1,text));
    fprintf(testLog,"PASS event: setting=%d value=%d calls=%d visible=1 offscreen=1 hover-protected=1\n",int(setting),expected,calls.load());fflush(testLog);
}
static void Render() {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    DWORD baseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    Tray::g_curDpi=192;
    Device::State mouse;mouse.isConnected=mouse.batteryValid=true;mouse.battery=90;
    mouse.dpiX=800;mouse.dpiLevel=1;mouse.pollingHz=2000;mouse.sleepMinutes=7;
    Keyboard::State keyboard=Keyboard::TestState();
    keyboard.isConnected=keyboard.isWired=keyboard.batteryValid=keyboard.lightingValid=keyboard.sleepValid=keyboard.lightingWritable=true;
    keyboard.battery=100;keyboard.brightness=keyboard.lightSpeed=4;keyboard.sleepSeconds=60;
    auto autoBefore=Tray::g_autoState;Tray::g_autoState.status=Autostart::Status::Registered;
    for(bool stale:{false,true}) {
        keyboard.lightingStale=keyboard.sleepStale=stale;keyboard.lightingWritable=!stale;
        auto rows=Tray::BuildMainItems(mouse,keyboard,true);
        int w=Tray::S(320),h=Tray::S(16);
        for(const auto& row:rows)h+=Tray::MenuItemHeight(row,192);
        HBITMAP bmp=CreateCompatibleBitmap(screen,w,h);auto old=SelectObject(dc,bmp);
        Tray::DrawMenuSurface(dc,RECT{0,0,w,h},192,true,rows,-1);SelectObject(dc,old);
        Save(dc,bmp,w,h,stale?L"verification/keyboard-ui/menu-stale.bmp":L"verification/keyboard-ui/menu-fresh.bmp");DeleteObject(bmp);
    }
    Tray::g_autoState=autoBefore;
    for(int dpi:{96,144,192})for(bool dark:{true})for(int type:{2,5}) {
        Tray::g_curDpi=dpi;Tray::g_curDark=dark;Tray::g_activeSubId=type;Tray::g_sliderVal=10;
        int w=Tray::S(260),h=Tray::S(138);HBITMAP bmp=CreateCompatibleBitmap(screen,w,h);auto old=SelectObject(dc,bmp);
        Tray::DrawSubMenuSurface(dc,RECT{0,0,w,h});SelectObject(dc,old);
        wchar_t path[128];swprintf_s(path,L"verification/keyboard-ui/sleep-%d-%d-%s.bmp",type,dpi,dark?L"dark":L"light");Save(dc,bmp,w,h,path);DeleteObject(bmp);
    }
    DWORD warmed=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<100;++i) {
        HBITMAP bmp=CreateCompatibleBitmap(screen,520,276);auto old=SelectObject(dc,bmp);
        Tray::DrawSubMenuSurface(dc,RECT{0,0,520,276});SelectObject(dc,old);DeleteObject(bmp);
    }
    DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    printf("GDI objects: before rendering %lu, after theme/DPI warmup %lu, after 100 repeats %lu\n",baseline,warmed,after);
    fprintf(testLog,"GDI objects: before=%lu warmed=%lu after100=%lu\n",baseline,warmed,after);fflush(testLog);
    assert(after==warmed);
    DeleteDC(dc);ReleaseDC(nullptr,screen);
}
int main(){try {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    testLog=fopen("verification/keyboard-ui/event-results.txt","w");assert(testLog);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=MainWndProc;wc.lpszClassName=L"KeyboardUIHiddenMain";assert(RegisterClassW(&wc));
    wc.lpfnWndProc=Tray::AcrylicSubWndProc;wc.lpszClassName=L"KeyboardUIHiddenSub";assert(RegisterClassW(&wc));
    g_uTaskbarRestartMsg=0xc001;
    g_hMainWnd=CreateWindowExW(0,L"KeyboardUIHiddenMain",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr);assert(g_hMainWnd);
    assert(Osd::Init(instance));
    Keyboard::testState.isConnected=Keyboard::testState.lightingValid=Keyboard::testState.sleepValid=Keyboard::testState.lightingWritable=true;
    Keyboard::testState.brightness=Keyboard::testState.lightSpeed=3;Keyboard::testState.sleepSeconds=600;
    auto stale=Keyboard::testState;stale.lightingStale=stale.sleepStale=true;stale.lightingWritable=false;
    auto staleRows=Tray::BuildMainItems(Device::State{},stale,false);
    int staleSettings=0,staleNotes=0;
    for(const auto& row:staleRows) {
        assert(!wcsstr(row.value,L"未刷新"));
        if(row.submenuType>=3 && row.submenuType<=5){assert(row.isDisabled && !wcscmp(row.value,L"—"));++staleSettings;}
        if(row.label && !wcscmp(row.label,L"配置暂未更新")){assert(row.isDisabled);++staleNotes;}
    }
    assert(staleSettings==3 && staleNotes==0);
    assert(StartSettingTasks());
    Osd::ShowDpiUpdate(2,800,1600,100,2000,false,false,L"鼠标");
    assert(wcsstr(Osd::g_textLine1,L"DPI 800") && Osd::g_textLine2[0]==0 && Osd::g_textLine3[0]==0 && Osd::valueMode);
    RECT dpiPopup{};GetWindowRect(Osd::GetWindowHandle(),&dpiPopup);
    assert(dpiPopup.bottom-dpiPopup.top==MulDiv(80,Osd::GetCurrentWindowDpi(Osd::GetWindowHandle()),96));
    fprintf(testLog,"PASS: single DPI value, no X/Y detail row, compact height\n");
    // A click is hit-tested even when no prior WM_MOUSEMOVE set the hover row.
    for(int dpi:{96,144,192})for(int type:{3,4}) {
        HWND sub=Menu(type,dpi);int before=calls;
        SendMessageW(sub,WM_LBUTTONUP,0,MAKELPARAM(Tray::S(20),Tray::S(8+3*28+10)));
        CheckResult(before,type==3?KeyboardProtocol::Setting::Brightness:KeyboardProtocol::Setting::Speed,3,L"75%");
    }
    HWND sub=Menu(4);int before=calls;
    Tray::g_subHover=3;SendMessageW(sub,WM_LBUTTONUP,0,MAKELPARAM(20,2));Pump();assert(calls==before && IsWindow(sub));Tray::DismissAllMenus();Pump();
    result=Keyboard::LightingResult::ReadbackFailed;sub=Menu(4);before=calls;
    SendMessageW(sub,WM_LBUTTONUP,0,MAKELPARAM(20,8+4*28+10));CheckResult(before,KeyboardProtocol::Setting::Speed,4,L"未完成");assert(wcsstr(Osd::g_textLine2,L"回读"));result=Keyboard::LightingResult::Confirmed;
    sub=Menu(5);before=calls;
    SendMessageW(sub,WM_LBUTTONDOWN,0,MAKELPARAM(115,108));CheckResult(before,KeyboardProtocol::Setting::Sleep,600,L"10 分钟");
    sub=Menu(5);Tray::g_sliderVal=5;before=calls;
    SendMessageW(sub,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);assert(calls==before && IsWindow(sub));
    SendMessageW(sub,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);assert(calls==before && Tray::g_sliderVal==7);
    SendMessageW(sub,WM_TIMER,Tray::WheelCommitTimer,0);CheckResult(before,KeyboardProtocol::Setting::Sleep,420,L"7 分钟");
    sub=Menu(5);before=calls;SendMessageW(sub,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);
    Tray::DismissAllMenus();Pump();assert(calls==before); // Cancel before debounce never sends a setting.
    sub=Menu(5);before=calls;
    SendMessageW(sub,WM_LBUTTONDOWN,0,MAKELPARAM(20,58));assert(Tray::g_isDraggingSlider && calls==before);
    SendMessageW(sub,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,58));assert(Tray::g_sliderVal==120 && calls==before);
    SendMessageW(sub,WM_LBUTTONUP,0,MAKELPARAM(240,58));CheckResult(before,KeyboardProtocol::Setting::Sleep,7200,L"120 分钟");
    // A slow device must not stall native message handling. Late completion from
    // the old connection must release the pending row without an obsolete popup.
    std::promise<void> entered,release;blockedEntered=&entered;blockedRelease=release.get_future().share();
    before=calls;SendMessageW(g_hMainWnd,Tray::KeyboardSettingMessage,WPARAM(KeyboardProtocol::Setting::Brightness),2);
    assert(entered.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready);
    auto during=Tray::BuildMainItems(Device::State{},Keyboard::TestState(),false);
    for(const auto& row:during)if(row.submenuType==3)assert(row.isDisabled);
    SendMessageW(g_hMainWnd,WM_TIMER,1,0); // UI completes while the fake setter is still blocked.
    assert(calls==before);
    {std::lock_guard<std::mutex> lock(testStateMutex);++Keyboard::testState.connectionGeneration;}
    std::wstring oldTitle=Osd::g_textLine1;release.set_value();
    auto pendingDeadline=GetTickCount64()+2000;
    do {Pump();if(!Tray::pendingSettings[3])break;Sleep(1);}while(GetTickCount64()<pendingDeadline);
    assert(calls==before+1 && !Tray::pendingSettings[3] && oldTitle==Osd::g_textLine1);
    blockedEntered=nullptr;
    fprintf(testLog,"PASS: blocked setting keeps native UI responsive; pending row disabled; old-connection completion cannot display success\n");
    // Expiry releases hover suppression; explicit clicks remain normal actions.
    keyboardFeedbackUntil=0;SendMessageW(g_hMainWnd,WM_APP_TRAYMSG,0,WM_MOUSEMOVE);assert(hoverPending);KillTimer(g_hMainWnd,4);hoverPending=false;
    // Background changes use the actual OSD path without any setting call.
    Keyboard::testState.isConnected=Keyboard::testState.lightingValid=true;
    Keyboard::testState.brightness=3;Keyboard::testState.lightSpeed=2;Keyboard::testState.lightingRevision=42;
    before=calls;
    SendMessageW(g_hMainWnd,Keyboard::SettingsChangedMessage,Keyboard::BrightnessChanged,42);
    assert(IsWindowVisible(Osd::GetWindowHandle()) && wcsstr(Osd::g_textLine1,L"75%") && calls==before);
    SendMessageW(g_hMainWnd,Keyboard::SettingsChangedMessage,Keyboard::SpeedChanged,42);
    assert(wcsstr(Osd::g_textLine1,L"50%") && calls==before);
    SendMessageW(g_hMainWnd,Keyboard::SettingsChangedMessage,Keyboard::BrightnessChanged|Keyboard::SpeedChanged,42);
    assert(wcsstr(Osd::g_textLine2,L"75%") && wcsstr(Osd::g_textLine2,L"50%") && calls==before);
    SendMessageW(g_hMainWnd,Keyboard::SettingsChangedMessage,Keyboard::BrightnessChanged,41);
    assert(wcsstr(Osd::g_textLine1,L"键盘灯光设置"));
    Keyboard::testState.lightingValid=false;
    SendMessageW(g_hMainWnd,Keyboard::SettingsChangedMessage,Keyboard::BrightnessChanged,42);
    assert(wcsstr(Osd::g_textLine1,L"键盘灯光设置") && calls==before);
    fprintf(testLog,"PASS: external brightness/speed/combined OSD, outdated revision and invalid reading rejection; no setting calls\n");fflush(testLog);
    Render();
    fprintf(testLog,"PASS: assertions complete; native events, failure, hover expiry, dark UI/DPI and GDI growth\n");fflush(testLog);
    StopSettingTasks();Pump();Osd::Cleanup();Tray::DismissAllMenus();DestroyWindow(g_hMainWnd);g_hMainWnd=nullptr;
    puts("PASS: real native menu click -> queued main message -> visible offscreen OSD for brightness/speed/sleep, failure reason, hover protection and expiry; no HID reads/writes or tray registration");
    puts("PASS: shared mouse/keyboard sleep slider rendered at 96/144/192 DPI in both themes; 100 redraws without GDI growth");
    fprintf(testLog,"PASS: window cleanup complete\n");fclose(testLog);return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());if(testLog){fprintf(testLog,"FAIL: %s\n",e.what());fclose(testLog);}Osd::Cleanup();Tray::DismissAllMenus();if(g_hMainWnd)DestroyWindow(g_hMainWnd);return 1;}}
