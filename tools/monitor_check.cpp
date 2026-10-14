// Bounded current-worker verification. No settings, keyboard text or tray UI.
// Stop immediately after the first published failed keyboard snapshot.
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
static std::atomic<unsigned> identityRequests{0},powerRequests{0},configRequests{0};
static std::atomic<bool> forbiddenWrite{false};
static BOOLEAN WINAPI MonitorSet(HANDLE h,PVOID data,ULONG size) {
    auto bytes=static_cast<BYTE*>(data);
    if(size!=520 || (bytes[1]!=0x82 && bytes[1]!=0x87 && bytes[1]!=0x84)) {
        forbiddenWrite=true;SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    if(bytes[1]==0x82)++identityRequests;
    if(bytes[1]==0x87)++powerRequests;
    if(bytes[1]==0x84)++configRequests;
    return HidD_SetFeature(h,data,size);
}
namespace Hid {
static bool MonitorTransfer(HANDLE h,BYTE* data,DWORD size,bool write,DWORD timeout,DWORD* received=nullptr) {
    if(write && size==20) {
        if(data[1]==5)++identityRequests;
        else if(data[1]==0x0B)++powerRequests;
        else if(data[1]==0x44)++configRequests;
        else {forbiddenWrite=true;SetLastError(ERROR_ACCESS_DENIED);return false;}
    }
    return Transfer(h,data,size,write,timeout,received);
}
}
#define HidD_SetFeature MonitorSet
#define Transfer MonitorTransfer
#define KEYBOARD_DIAGNOSTIC
#include "../src/keyboard_manager.cpp"
#undef KEYBOARD_DIAGNOSTIC
#undef HidD_SetFeature
#undef Transfer
#include "../src/device_manager.cpp"
static void MouseChanged(const Device::State&,DWORD){}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    auto seconds=std::clamp(atoi(argv[1]),1,90);
    setvbuf(stdout,nullptr,_IONBF,0);
    auto window=CreateWindowExW(0,L"STATIC",L"isolated-monitor",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)return 3;
    // Only keyboard's normal read-only deadlines; mouse passive state input.
    if(!Device::Start(nullptr,MouseChanged,false)){DestroyWindow(window);return 4;}
    if(!Keyboard::Start(window,WM_APP+2)){Device::Stop();DestroyWindow(window);return 4;}
    const auto start=GetTickCount64(),until=start+seconds*1000;
    bool failed=false,seen=false;unsigned snapshots=0;
    printf("monitor_start seconds=%d pid=%lu setting_writes=0\n",seconds,GetCurrentProcessId());
    while(GetTickCount64()<until) {
        MSG message;
        while(PeekMessageW(&message,window,0,0,PM_REMOVE)) {
            if(message.message!=WM_APP+2)continue;
            auto state=Keyboard::GetCurrentState();seen=true;++snapshots;
            printf("snapshot elapsed_ms=%llu connected=%d wired=%d battery_valid=%d brightness=%d speed=%d sleep=%d lighting_valid=%d stale=%d result=%d\n",
                GetTickCount64()-start,state.isConnected,state.isWired,state.batteryValid,state.brightness,state.lightSpeed,state.sleepSeconds,state.lightingValid,state.lightingStale,int(state.configResult));
            if(!state.isConnected || !state.batteryValid || !state.lightingValid || !state.sleepValid || state.lightingStale || state.sleepStale || forbiddenWrite) {failed=true;break;}
        }
        if(failed)break;
        if(!seen && GetTickCount64()-start>5000){failed=true;puts("first_snapshot_timeout stop=1");break;}
        Sleep(25);
    }
    auto keyboard=Keyboard::GetCurrentState();auto mouse=Device::GetCurrentState();
    Keyboard::Stop();Device::Stop();DestroyWindow(window);
    printf("monitor_complete failed=%d snapshots=%u identity_requests=%u power_requests=%u config_requests=%u mouse_connected=%d mouse_battery_valid=%d keyboard_battery=%d setting_writes=0\n",
        failed,snapshots,identityRequests.load(),powerRequests.load(),configRequests.load(),mouse.isConnected,mouse.batteryValid,keyboard.battery);
    return failed?5:0;
}
