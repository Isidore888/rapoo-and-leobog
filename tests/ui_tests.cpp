#include "../src/alert_window.h"
#include "../src/tray_menu.h"
#include "../src/osd_window.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <stdexcept>
#include <cstring>
#undef assert
#define assert(condition) do { if(!(condition))throw std::runtime_error(#condition); } while(0)
struct RegistryBackup {
    const wchar_t* name; DWORD type=0; std::vector<BYTE> data; bool existed=false;
    RegistryBackup(const wchar_t* value):name(value) {
        HKEY k;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\rapoo-tray",0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)return;
        DWORD size=0;if(RegQueryValueExW(k,name,nullptr,&type,nullptr,&size)==ERROR_SUCCESS){data.resize(size);existed=RegQueryValueExW(k,name,nullptr,&type,data.data(),&size)==ERROR_SUCCESS;}
        RegCloseKey(k);
    }
    void Restore() {
        HKEY k;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\rapoo-tray",0,KEY_SET_VALUE,&k)!=ERROR_SUCCESS)return;
        if(existed)RegSetValueExW(k,name,0,type,data.data(),DWORD(data.size()));else RegDeleteValueW(k,name);
        RegCloseKey(k);
    }
    ~RegistryBackup(){Restore();}
};
static HWND validationOwner=nullptr;
static void CALLBACK DismissValidation(HWND,UINT,UINT_PTR timer,DWORD) {
    HWND dialog=GetLastActivePopup(validationOwner);
    DWORD process=0;if(dialog)GetWindowThreadProcessId(dialog,&process);
    if(dialog!=validationOwner && process==GetCurrentProcessId()){PostMessageW(dialog,WM_COMMAND,IDOK,0);KillTimer(nullptr,timer);}
}
static void Pump() { MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(Alert::HandleSettingsMessage(&msg))continue;TranslateMessage(&msg);DispatchMessageW(&msg);} }
static void Capture(HWND window,const wchar_t* path) {
    RECT r;GetWindowRect(window,&r);int w=r.right-r.left,h=r.bottom-r.top;
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bmp=CreateCompatibleBitmap(screen,w,h);auto old=SelectObject(dc,bmp);
    assert(PrintWindow(window,dc,0));
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    std::vector<BYTE> pixels(w*h*4);SelectObject(dc,old);assert(GetDIBits(dc,bmp,0,h,pixels.data(),&info,DIB_RGB_COLORS));
    BITMAPFILEHEADER file{};file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+DWORD(pixels.size());
    FILE* out=_wfopen(path,L"wb");assert(out);fwrite(&file,sizeof(file),1,out);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out);fwrite(pixels.data(),pixels.size(),1,out);fclose(out);
    DeleteObject(bmp);DeleteDC(dc);ReleaseDC(nullptr,screen);
}
static void SaveSurface(HDC dc,int w,int h,const wchar_t* path) {
    HDC screen=GetDC(nullptr),copy=CreateCompatibleDC(screen);HBITMAP bmp=CreateCompatibleBitmap(screen,w,h);auto old=SelectObject(copy,bmp);
    assert(BitBlt(copy,0,0,w,h,dc,0,0,SRCCOPY));SelectObject(copy,old);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    std::vector<BYTE> pixels(w*h*4);assert(GetDIBits(copy,bmp,0,h,pixels.data(),&info,DIB_RGB_COLORS));
    BITMAPFILEHEADER file{};file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+DWORD(pixels.size());
    FILE* out=_wfopen(path,L"wb");assert(out);fwrite(&file,sizeof(file),1,out);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out);fwrite(pixels.data(),pixels.size(),1,out);fclose(out);
    DeleteObject(bmp);DeleteDC(copy);ReleaseDC(nullptr,screen);
}
#include "../src/tray_menu.cpp"
static void VerifyMenuSync() {
    Device::State mouse;mouse.isConnected=true;mouse.pollingHz=2000;
    Keyboard::State keyboard;keyboard.isConnected=keyboard.lightingValid=keyboard.lightingWritable=keyboard.sleepValid=true;
    keyboard.brightness=keyboard.lightSpeed=4;keyboard.sleepSeconds=300;
    Tray::SetMenuSnapshot(mouse,keyboard,false);
    keyboard.brightness=3;keyboard.lightSpeed=1;keyboard.sleepSeconds=600;
    Tray::SetMenuSnapshot(mouse,keyboard,false);
    for(const auto& item:Tray::g_mainItems) {
        if(item.submenuType==3)assert(wcsstr(item.value,L"75%") && Tray::g_menuKeyboard.brightness==3);
        if(item.submenuType==4)assert(wcsstr(item.value,L"25%") && Tray::g_menuKeyboard.lightSpeed==1);
        if(item.submenuType==5)assert(wcsstr(item.value,L"10 分钟") && Tray::g_menuKeyboard.sleepSeconds==600);
    }
    keyboard.lightingValid=keyboard.sleepValid=false;Tray::SetMenuSnapshot(mouse,keyboard,false);
    for(const auto& item:Tray::g_mainItems)if(item.submenuType>=3)assert(wcsstr(item.value,L"—"));
    mouse.pollingHz=1000;Tray::SetMenuSnapshot(mouse,keyboard,false);assert(Tray::g_menuMouse.pollingHz==1000);
    puts("PASS: main menu values and submenu state share updated brightness/speed/sleep snapshot, including invalidation; no windows");
}
static void VerifyVisuals() {
    VerifyMenuSync();
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bmp=CreateCompatibleBitmap(screen,640,480);auto old=SelectObject(dc,bmp);
    for(int style=0;style<3;++style) {
        for(int theme=0;theme<2;++theme) {
            int y=(theme*3+style)*80;HBRUSH bg=CreateSolidBrush(theme?RGB(24,28,36):RGB(250,251,254));RECT row{0,y,640,y+80};FillRect(dc,&row,bg);DeleteObject(bg);
            int x=16;
            for(int size:{16,20,24,32,48,64}) {
                HICON icon=Tray::CreateBatteryIcon(100,false,true,size,theme!=0,57,false,true,style);assert(icon);
                assert(DrawIconEx(dc,x,y+8,icon,size,size,0,nullptr,DI_NORMAL));DestroyIcon(icon);x+=size+16;
            }
            for(auto state:{0,1,2}) {
                HICON icon=Tray::CreateBatteryIcon(state==0?0:100,state==1,true,32,theme!=0,state==2?-1:5,false,state!=2,style);assert(icon);
                assert(DrawIconEx(dc,x,y+8,icon,32,32,0,nullptr,DI_NORMAL));DestroyIcon(icon);x+=48;
            }
        }
    }
    SaveSurface(dc,640,480,L"verification/render/icon-styles.bmp");
    SelectObject(dc,old);DeleteObject(bmp);
    Device::State mouse;mouse.isConnected=mouse.batteryValid=true;mouse.battery=100;mouse.pollingHz=2000;mouse.sleepMinutes=7;
    Keyboard::State keyboard;keyboard.isConnected=keyboard.batteryValid=keyboard.isWired=keyboard.isCharging=keyboard.chargingKnown=true;keyboard.battery=100;keyboard.lightingValid=true;keyboard.brightness=3;keyboard.lightSpeed=3;keyboard.sleepValid=true;keyboard.sleepSeconds=600;
    for(int dpi:{96,144,192})for(bool dark:{true}) {
        int w=MulDiv(360,dpi,96),h=MulDiv(298,dpi,96);bmp=CreateCompatibleBitmap(screen,w,h);old=SelectObject(dc,bmp);
        Osd::DrawStatus(dc,dpi,dark,mouse,keyboard);wchar_t path[128];swprintf_s(path,L"verification/render/status-%d-%s.bmp",dpi,dark?L"dark":L"light");SaveSurface(dc,w,h,path);
        SelectObject(dc,old);DeleteObject(bmp);
    }
    for(int dpi:{96,144,192})for(bool dark:{true}) {
        keyboard.lightingWritable=true;
        bmp=CreateCompatibleBitmap(screen,MulDiv(320,dpi,96),MulDiv(500,dpi,96));old=SelectObject(dc,bmp);
        auto size=Tray::DrawMenuPreview(dc,dpi,dark,mouse,keyboard);assert(size.cy<=MulDiv(500,dpi,96));
        wchar_t path[128];swprintf_s(path,L"verification/render/menu-%d-%s.bmp",dpi,dark?L"dark":L"light");SaveSurface(dc,size.cx,size.cy,path);
        SelectObject(dc,old);DeleteObject(bmp);
    }
    DeleteDC(dc);ReleaseDC(nullptr,screen);
}
int RunTests() {
    RegistryBackup alerts(L"DualBatteryAlerts"),style(L"DualBatteryStyleV2");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto instance=GetModuleHandleW(nullptr);Alert::Init(instance);
    VerifyVisuals();
    HWND owner=CreateWindowExW(0,L"STATIC",L"Verification owner",WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr);
    Alert::ShowSettings(owner);Pump();HWND settings=FindWindowW(L"DualBatteryAlertSettings",nullptr);assert(settings);
    Capture(settings,L"verification/settings.bmp");
    SetFocus(GetDlgItem(settings,200));MSG tab{};tab.hwnd=GetDlgItem(settings,200);tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;
    assert(Alert::HandleSettingsMessage(&tab));assert(GetFocus()==GetDlgItem(settings,211));
    SetDlgItemTextW(settings,201,L"10");SetDlgItemTextW(settings,202,L"10");
    validationOwner=settings;auto timer=SetTimer(nullptr,0,100,DismissValidation);
    SendMessageW(settings,WM_COMMAND,IDOK,0);KillTimer(nullptr,timer);
    assert(IsWindow(settings)&&GetFocus()==GetDlgItem(settings,201));

    SetDlgItemTextW(settings,201,L"30");SetDlgItemTextW(settings,202,L"15");SetDlgItemTextW(settings,211,L"40");SetDlgItemTextW(settings,212,L"5");
    SendDlgItemMessageW(settings,200,BM_SETCHECK,BST_UNCHECKED,0);SendDlgItemMessageW(settings,210,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(settings,WM_COMMAND,IDOK,0);Pump();assert(!IsWindow(settings));
    assert(!Alert::GetConfig(0).enabled&&Alert::GetConfig(0).warning==30&&Alert::GetConfig(1).warning==40&&Alert::GetConfig(1).critical==5);
    Alert::Cleanup();Alert::Init(instance);assert(Alert::GetConfig(0).warning==30&&Alert::GetConfig(1).critical==5);
    Alert::ShowSettings(owner);Pump();settings=FindWindowW(L"DualBatteryAlertSettings",nullptr);SetDlgItemTextW(settings,201,L"80");SendMessageW(settings,WM_COMMAND,IDCANCEL,0);assert(Alert::GetConfig(0).warning==30);
    AlertPolicy::Config defaults[2];assert(Alert::SaveConfig(defaults));
    Device::State mouse;mouse.isConnected=mouse.batteryValid=true;mouse.battery=20;
    Keyboard::State keyboard;keyboard.isConnected=keyboard.batteryValid=true;keyboard.battery=5;
    Alert::CheckBattery(mouse,keyboard);Pump();HWND popup;
    popup=FindWindowW(nullptr,mouse.modelName);assert(popup&&IsWindowVisible(popup));
    Capture(popup,L"verification/mouse-alert.bmp");Alert::Hide();Pump();
    popup=FindWindowW(nullptr,keyboard.modelName);assert(popup&&IsWindowVisible(popup));Capture(popup,L"verification/keyboard-alert.bmp");
    keyboard.isCharging=keyboard.chargingKnown=true;Alert::CheckBattery(mouse,keyboard);assert(!IsWindowVisible(popup));
    Alert::CheckBattery(mouse,keyboard);assert(!IsWindowVisible(popup));
    for(int theme=0;theme<2;++theme)for(int size:{16,20,24,32,48,64}){
        HICON icon=Tray::CreateBatteryIcon(0,false,true,size,theme!=0,57,false,true);assert(icon);DestroyIcon(icon);
        icon=Tray::CreateBatteryIcon(-1,false,false,size,theme!=0,-1,false,false);assert(icon);DestroyIcon(icon);
    }
    Alert::Cleanup();DestroyWindow(owner);puts("PASS: native settings save/reload/cancel, per-device alert queue/charging, tray icon sizes/themes; registry restored");
    return 0;
}

int main(int argc,char** argv){try{
    if(argc>1 && strcmp(argv[1],"--render-only")==0) {
        VerifyVisuals();puts("PASS: offscreen dual icon styles, battery boundaries, dark status rings and dual-device menus at 96/144/192 DPI; no windows or hardware access");return 0;
    }
    return RunTests();
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
