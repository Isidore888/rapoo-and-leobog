#include <windows.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include "../src/alert_window.h"
#include "../src/alert_window.cpp"
static int saves=0;
static bool saveAllowed=true;
static AlertPolicy::Config saved[2];
namespace Alert {
bool TestSaveConfig(const AlertPolicy::Config (&config)[2]) {
    assert(AlertPolicy::Valid(config[0]) && AlertPolicy::Valid(config[1]));
    if(!saveAllowed)return false;
    ++saves;for(int i=0;i<2;++i)saved[i]=config[i];return true;
}
}
static BOOL WINAPI TestShow(HWND h,int mode){
    if(GetWindowLongPtrW(h,GWL_STYLE)&WS_CHILD)return ::ShowWindow(h,mode);
    SetWindowPos(h,nullptr,GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)+10000,10000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    return ::ShowWindow(h,SW_SHOWNOACTIVATE);
}
static BOOL WINAPI TestForeground(HWND){return TRUE;}
static BOOL WINAPI TestPosition(HWND h,HWND after,int x,int y,int w,int height,UINT flags) {
    if(!(GetWindowLongPtrW(h,GWL_STYLE)&WS_CHILD)) {
        x=GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)+10000;y=10000;
        flags=(flags&~SWP_NOMOVE)|SWP_NOACTIVATE;
    }
    return ::SetWindowPos(h,after,x,y,w,height,flags);
}
#define SaveConfig TestSaveConfig
#define ShowWindow TestShow
#define SetForegroundWindow TestForeground
#define SetWindowPos TestPosition
#include "../src/alert_settings.cpp"
#undef SetWindowPos
#undef SetForegroundWindow
#undef ShowWindow
#undef SaveConfig
#include "../src/tray_menu.cpp"
#include "../src/osd_window.cpp"

template<class Draw> static void Render(const wchar_t* path,int w,int h,Draw draw) {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);assert(bitmap);auto old=SelectObject(dc,bitmap);draw(dc);GdiFlush();
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+w*h*4;
    FILE* out=_wfopen(path,L"wb");assert(out);fwrite(&file,sizeof(file),1,out);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out);fwrite(pixels,w*h*4,1,out);fclose(out);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
}
static void PrintSettings(HWND h,HDC dc) {
    SendMessageW(h,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);
    for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT r;GetWindowRect(child,&r);MapWindowPoints(nullptr,h,(POINT*)&r,2);
        int saved=SaveDC(dc);SetViewportOrgEx(dc,r.left,r.top,nullptr);SendMessageW(child,WM_PRINT,(WPARAM)dc,PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(dc,saved);
    }
}
int main(int argc,char** argv) {
    std::filesystem::create_directories("verification/ui-selected-20261013");
    std::filesystem::create_directories("verification/alert-settings-layout4-20261014");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!(argc>1 && !std::strcmp(argv[1],"--settings-only"))) {
    Device::State mouse;mouse.isConnected=mouse.batteryValid=true;mouse.battery=90;mouse.dpiX=800;mouse.pollingHz=2000;mouse.sleepMinutes=7;
    Keyboard::State keys;keys.isConnected=keys.isWired=keys.batteryValid=keys.lightingValid=keys.sleepValid=keys.lightingWritable=true;keys.battery=100;keys.brightness=keys.lightSpeed=4;keys.sleepSeconds=60;
    DWORD warmed=0;
    Tray::g_autoState.status=Autostart::Status::Registered; // screenshot example only
    for(int dpi:{96,144,192}) {
        for(int state=0;state<3;++state) {
            auto k=keys;k.lightingStale=k.sleepStale=state==1;k.isConnected=state!=2;
            auto rows=Tray::BuildMainItems(mouse,k,true);int height=Ui::Scale(16,dpi);
            for(const auto& item:rows) {
                height+=Tray::MenuItemHeight(item,dpi);
                assert(!item.label || !wcsstr(item.label,L"配置暂未更新"));
                if(state && item.submenuType>=3)assert(item.isDisabled && !wcscmp(item.value,L"—"));
                if(item.isModel)assert(!item.value[0]); // connection is its own aligned chip
            }
            wchar_t path[150];swprintf_s(path,L"verification/ui-selected-20261013/menu-%d-%d.bmp",dpi,state);
            Render(path,Ui::Scale(320,dpi),height,[&](HDC dc){Tray::DrawMenuSurface(dc,RECT{0,0,Ui::Scale(320,dpi),height},dpi,true,rows,-1);});
            swprintf_s(path,L"verification/ui-selected-20261013/status-%d-%d.bmp",dpi,state);
            Render(path,Ui::Scale(360,dpi),Ui::Scale(298,dpi),[&](HDC dc){Osd::DrawStatus(dc,dpi,true,mouse,k);});
        }
        for(bool keyboard:{false,true}) {
            wchar_t path[150];swprintf_s(path,L"verification/ui-selected-20261013/value-%d-%d.bmp",dpi,keyboard);
            Render(path,Ui::Scale(360,dpi),Ui::Scale(80,dpi),[&](HDC dc){Osd::DrawValue(dc,dpi,keyboard?L"键盘亮度":L"DPI",keyboard?L"75%":L"800",keyboard);});
        }
        wchar_t path[150];swprintf_s(path,L"verification/ui-selected-20261013/alert-%d.bmp",dpi);
        Render(path,Ui::Scale(390,dpi),Ui::Scale(150,dpi),[&](HDC dc){Alert::DrawAlert(dc,dpi,L"LEOBOG HI98C Pro",15,false);});
        Tray::g_curDpi=dpi;Tray::g_curDark=true;Tray::g_activeSubId=5;Tray::g_sliderVal=120;
        swprintf_s(path,L"verification/ui-selected-20261013/sleep-%d.bmp",dpi);
        Render(path,Ui::Scale(260,dpi),Ui::Scale(138,dpi),[&](HDC dc){Tray::DrawSubMenuSurface(dc,RECT{0,0,Ui::Scale(260,dpi),Ui::Scale(138,dpi)});});
    }
    puts("PASS: selected native layouts, aligned modes, stale/disconnected dashes at 96/144/192 DPI; no HID");
    warmed=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int n=0;n<25;++n)Render(L"verification/ui-selected-20261013/repeated.bmp",360,298,[&](HDC dc){Osd::DrawStatus(dc,96,true,mouse,keys);});
    assert(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==warmed);
    puts("PASS: repeated status rendering returns GDI resources to warmed baseline");
    }
    // Only local test drafts are saved. Registry persistence is replaced above.
    Alert::configs[0]={true,20,10};Alert::configs[1]={false,25,5};
    // USER/GDI initializes process-wide caches on the first native page.
    DWORD coldGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    Alert::ShowSettings(nullptr);RedrawWindow(Alert::settingsWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);Alert::CloseSettings();
    DWORD initialGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    printf("GDI cold=%lu warmed=%lu\n",coldGdi,initialGdi);fflush(stdout);
    Alert::ShowSettings(nullptr);HWND h=Alert::settingsWindow;assert(h);

    assert(!GetWindow(h,GW_OWNER) && (GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_APPWINDOW));
    assert(GetWindowLongPtrW(h,GWL_STYLE)&WS_MINIMIZEBOX);assert(!GetDlgItem(h,300) && !GetDlgItem(h,301));
    assert(GetWindowLongPtrW(GetDlgItem(h,201),GWL_STYLE)&WS_VISIBLE);
    assert(GetWindowLongPtrW(GetDlgItem(h,211),GWL_STYLE)&WS_VISIBLE);assert(!IsWindowEnabled(GetDlgItem(h,211)));
    // Native Tab order follows the table row, then the next enabled row.
    assert(GetNextDlgTabItem(h,GetDlgItem(h,201),FALSE)==GetDlgItem(h,202));
    assert(GetNextDlgTabItem(h,GetDlgItem(h,202),FALSE)==GetDlgItem(h,200));
    SetDlgItemTextW(h,201,L"30");SetDlgItemTextW(h,202,L"15");
    SendDlgItemMessageW(h,210,BM_SETCHECK,BST_CHECKED,0);SendMessageW(h,WM_COMMAND,MAKEWPARAM(210,BN_CLICKED),0);
    assert(IsWindowEnabled(GetDlgItem(h,211)));assert(GetNextDlgTabItem(h,GetDlgItem(h,200),FALSE)==GetDlgItem(h,211));
    SetDlgItemTextW(h,211,L"40");SetDlgItemTextW(h,212,L"5");
    SetFocus(h);
    for(int dpi:{96,144,192}) {
        RECT suggested;GetWindowRect(h,&suggested);SendMessageW(h,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&suggested);
        assert(GetDlgItemInt(h,201,nullptr,FALSE)==30 && GetDlgItemInt(h,211,nullptr,FALSE)==40);
        // Synthetic WM_DPICHANGED does not change Windows' actual nonclient DPI.
        RECT outer{},client{};GetWindowRect(h,&outer);GetClientRect(h,&client);
        SetWindowPos(h,nullptr,0,0,Ui::Scale(560,dpi)+(outer.right-outer.left-client.right),Ui::Scale(360,dpi)+(outer.bottom-outer.top-client.bottom),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        GetClientRect(h,&client);assert(client.right==Ui::Scale(560,dpi) && client.bottom==Ui::Scale(360,dpi));
        for(int id:{200,201,202,210,211,212,IDOK,IDCANCEL}) {
            RECT bounds{};GetWindowRect(GetDlgItem(h,id),&bounds);MapWindowPoints(nullptr,h,(POINT*)&bounds,2);
            assert(bounds.left>=0 && bounds.right<=client.right && bounds.top>=0 && bounds.bottom<=client.bottom);
        }
        for(int id:{201,202,211,212}) {
            HWND edit=GetDlgItem(h,id);assert(GetWindowLongPtrW(edit,GWL_STYLE)&ES_CENTER);
            RECT text{},field{};SendMessageW(edit,EM_GETRECT,0,(LPARAM)&text);GetClientRect(edit,&field);
            assert(abs(text.left+text.right-field.right)<=1 && abs(text.top+text.bottom-field.bottom)<=1);
            RECT bounds{};GetWindowRect(edit,&bounds);MapWindowPoints(nullptr,h,(POINT*)&bounds,2);
            assert(bounds.left>=0 && bounds.right<=client.right && bounds.top>=0 && bounds.bottom<=client.bottom);
        }
        wchar_t path[150];swprintf_s(path,L"verification/alert-settings-layout4-20261014/settings-%d.bmp",dpi);
        Render(path,client.right,client.bottom,[&](HDC dc){PrintSettings(h,dc);});
    }
    SetDlgItemTextW(h,202,L"30");SendMessageW(h,WM_COMMAND,IDOK,0);
    assert(!saves && Alert::errorField==202 && GetFocus()==GetDlgItem(h,202) && IsWindow(h));
    RECT client{};GetClientRect(h,&client);Render(L"verification/alert-settings-layout4-20261014/settings-error.bmp",client.right,client.bottom,[&](HDC dc){PrintSettings(h,dc);});
    SetDlgItemTextW(h,202,L"15");
    // Centered multiline formatting must not let pasted junk parse as a valid threshold.
    for(const wchar_t* value:{L"40x",L"4\n0",L"0",L"101",L""}) {
        SetDlgItemTextW(h,211,value);SendMessageW(h,WM_COMMAND,IDOK,0);assert(!saves && Alert::errorField==211 && IsWindow(h));
    }
    SetDlgItemTextW(h,211,L"40");saveAllowed=false;SendMessageW(h,WM_COMMAND,IDOK,0);
    assert(!saves && IsWindow(h) && Alert::settingsError.find(L"保存失败")!=std::wstring::npos);
    saveAllowed=true;MSG enter{};enter.hwnd=GetDlgItem(h,201);enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
    assert(Alert::HandleSettingsMessage(&enter) && saves==1 && !IsWindow(h));
    assert(saved[0].warning==30 && saved[0].critical==15 && saved[1].warning==40 && saved[1].critical==5 && saved[1].enabled);
    Alert::ShowSettings(nullptr);h=Alert::settingsWindow;SetDlgItemTextW(h,201,L"90");
    MSG escape{};escape.hwnd=GetDlgItem(h,201);escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;
    assert(Alert::HandleSettingsMessage(&escape) && saves==1 && !IsWindow(h));
    Alert::ShowSettings(nullptr);h=Alert::settingsWindow;MSG toggle{};toggle.hwnd=GetDlgItem(h,210);toggle.message=WM_KEYDOWN;toggle.wParam=VK_RETURN;
    assert(Alert::HandleSettingsMessage(&toggle) && SendDlgItemMessageW(h,210,BM_GETCHECK,0,0)==BST_CHECKED && saves==1);
    Alert::ShowSettings(nullptr);assert(Alert::settingsWindow==h);Alert::Cleanup();assert(!IsWindow(h));
    assert(!Alert::settingsFont && !Alert::fieldBrush && !Alert::settingsWindow);
    printf("GDI after suite=%lu\n",GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS));fflush(stdout);
    for(int n=0;n<10;++n){Alert::ShowSettings(nullptr);Alert::CloseSettings();assert(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==initialGdi);}
    printf("GDI after DPI changes and10 reopen/close cycles=%lu\n",GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS));
    assert(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==initialGdi);
    puts("PASS: independent settings page/table, both drafts visible, centered native text at96/144/192DPI, DPI change preserves drafts, disable/Tab/Enter/Esc, strict validation, failed save, cleanup/GDI; no registry or HID writes");
    return 0;
}
