// Bounded production keyboard worker verification. Known queries only, no mouse
// initialization, key input, physical setting writes or startup registry access.
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <atomic>
#include <cstdio>
#include <cstring>
static std::atomic<unsigned> identities{0},powers{0},configs{0};
static std::atomic<bool> forbidden{false};
static bool Request(BYTE command,bool wired) {
    if(command==(wired?0x82:5))++identities;
    else if(command==(wired?0x87:0x0B))++powers;
    else if(command==(wired?0x84:0x44))++configs;
    else {forbidden=true;return false;}
    if(identities+powers+configs>64){forbidden=true;return false;}return true;
}
static BOOLEAN WINAPI WorkerFeature(HANDLE h,PVOID data,ULONG size) {
    if(size!=520 || !Request(static_cast<BYTE*>(data)[1],true)){forbidden=true;return FALSE;}
    return HidD_SetFeature(h,data,size);
}
namespace Hid {
static bool WorkerTransfer(HANDLE h,BYTE* data,DWORD size,bool write,DWORD timeout,DWORD* received=nullptr) {
    if(write && (size!=20 || !Request(data[1],false))){forbidden=true;return false;}
    return Transfer(h,data,size,write,timeout,received);
}
}
#define HidD_SetFeature WorkerFeature
#define Transfer WorkerTransfer
#define KEYBOARD_DIAGNOSTIC
#include "../src/keyboard_manager.cpp"
#undef KEYBOARD_DIAGNOSTIC
#undef HidD_SetFeature
#undef Transfer
int main(int argc,char** argv) {
    if(argc!=2 || (strcmp(argv[1],"--usb") && strcmp(argv[1],"--radio")))return 2;
    const bool wired=!strcmp(argv[1],"--usb");setvbuf(stdout,nullptr,_IONBF,0);
    HWND window=CreateWindowExW(0,L"STATIC",L"keyboard-only-worker-check",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window || !Keyboard::Start(window,WM_APP+2)){if(window)DestroyWindow(window);return 3;}
    const auto start=GetTickCount64(),deadline=start+30000;bool seen=false,failed=false;unsigned snapshots=0;
    printf("worker_check_start seconds=30 mode=%s request_limit=64 setting_writes=0 mouse_initialized=0\n",wired?"usb":"radio");
    while(GetTickCount64()<deadline) {
        MSG message;
        while(PeekMessageW(&message,window,0,0,PM_REMOVE)) {
            if(message.message!=WM_APP+2)continue;
            const auto state=Keyboard::GetCurrentState();seen=true;++snapshots;
            printf("snapshot elapsed=%llu mode=%s battery=%d brightness=%d speed=%d effect=%d sleep=%d revision=%llu valid=%d\n",
                GetTickCount64()-start,state.isWired?"usb":"radio",state.battery,state.brightness,state.lightSpeed,state.lightEffect,state.sleepSeconds,
                state.lightingRevision,state.batteryValid && state.lightingValid && state.sleepValid);
            if(!state.isConnected || state.isWired!=wired || !state.batteryValid || !state.lightingValid || !state.sleepValid ||
                state.lightingStale || state.sleepStale || forbidden){failed=true;break;}
        }
        if(failed)break;
        if(!seen && GetTickCount64()-start>5000){failed=true;puts("snapshot_timeout stop=1");break;}
        Sleep(20);
    }
    Keyboard::Stop();DestroyWindow(window);
    printf("worker_check_complete failed=%d snapshots=%u identity=%u power=%u config=%u setting_writes=0 forbidden=%d\n",
        failed,snapshots,identities.load(),powers.load(),configs.load(),forbidden.load());
    return failed?4:0;
}
