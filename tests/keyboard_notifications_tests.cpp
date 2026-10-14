#include "../src/hid_transport.h"
#include <cassert>
#include <cstdio>
#include <array>
#include <algorithm>
static bool wrongEndpoint=false,openFails=false,synchronous=false,shortReport=false,rearmFails=false;
static unsigned reads=0,cancels=0,drains=0;
static OVERLAPPED* activeIo=nullptr;
static BYTE* activeBuffer=nullptr;
namespace Hid {
std::vector<Endpoint> NotificationEndpoints() {
    Endpoint ep;ep.path=L"isolated-notification";ep.vid=0x258A;ep.pid=wrongEndpoint?0xFFFF:0x010C;
    ep.caps.UsagePage=0xFF00;ep.caps.Usage=1;ep.caps.InputReportByteLength=4;return {ep};
}
}
static HANDLE WINAPI NotificationOpen(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES,DWORD create,DWORD flags,HANDLE) {
    assert((!wcscmp(name,L"isolated-notification") || !wcscmp(name,L"isolated-radio")) && access==GENERIC_READ && share==(FILE_SHARE_READ|FILE_SHARE_WRITE));
    assert(create==OPEN_EXISTING && flags==FILE_FLAG_OVERLAPPED);
    return openFails?INVALID_HANDLE_VALUE:CreateEventW(nullptr,TRUE,FALSE,nullptr);
}
static BOOL WINAPI NotificationRead(HANDLE,LPVOID buffer,DWORD size,LPDWORD count,LPOVERLAPPED io) {
    assert((size==4 || size==20) && io && io->hEvent);++reads;
    if(rearmFails && reads==2){SetLastError(ERROR_DEVICE_NOT_CONNECTED);return FALSE;}
    activeIo=io;activeBuffer=static_cast<BYTE*>(buffer);
    const BYTE hint[]={3,8,0,1};
    const BYTE radio[]={0x13,0x0A,1,0,4,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0x29};
    if(size==4)std::copy(hint,hint+4,activeBuffer);else std::copy(radio,radio+20,activeBuffer);
    *count=shortReport?size-1:size;if(synchronous)return TRUE;
    SetLastError(ERROR_IO_PENDING);return FALSE;
}
static BOOL WINAPI NotificationResult(HANDLE,LPOVERLAPPED io,LPDWORD count,BOOL wait) {
    assert(io==activeIo && activeBuffer);const DWORD size=activeBuffer[0]==0x13?20:4;*count=shortReport?size-1:size;
    if(wait){++drains;assert(cancels>0);SetLastError(ERROR_OPERATION_ABORTED);return FALSE;}
    return TRUE;
}
static BOOL WINAPI NotificationCancel(HANDLE,LPOVERLAPPED io){assert(io==activeIo);++cancels;return TRUE;}
#define Enumerate NotificationEndpoints
#define CreateFileW NotificationOpen
#define ReadFile NotificationRead
#define GetOverlappedResult NotificationResult
#define CancelIoEx NotificationCancel
#include "../src/keyboard_notifications.h"
#undef Enumerate
#undef CreateFileW
#undef ReadFile
#undef GetOverlappedResult
#undef CancelIoEx
int main() {
    using KeyboardNotifications::Input;
    DWORD count=0;std::array<BYTE,4> hint{};
    const std::array<BYTE,4> expected={3,8,0,1};
    {
        Input input;assert(input.Attach() && input.Event());
        assert(input.Consume(hint,count) && count==4 && hint==expected);
        assert(reads==2);input.Close();assert(!input.Event() && cancels==1 && drains==1);
        input.Close();assert(cancels==1 && drains==1);
    }
    reads=0;wrongEndpoint=true;{Input input;assert(!input.Attach() && reads==0);}wrongEndpoint=false;
    openFails=true;{Input input;assert(!input.Attach() && reads==0);}openFails=false;
    shortReport=true;{Input input;assert(input.Attach() && !input.Consume(hint,count) && !input.Event());}shortReport=false;
    assert(cancels==1 && drains==1);
    reads=0;synchronous=true;
    {Input input;assert(input.Attach() && input.Consume(hint,count));}synchronous=false;
    assert(cancels==1 && drains==1);
    reads=0;rearmFails=true;
    {Input input;assert(input.Attach() && !input.Consume(hint,count) && !input.Event());}rearmFails=false;
    assert(cancels==1 && drains==1);
    reads=0;
    {
        KeyboardNotifications::RadioInput input;
        assert(input.Configure(L"isolated-radio") && input.Event());
        const auto stableEvent=input.Event();std::array<BYTE,20> packet{};
        SetEvent(stableEvent);assert(input.Consume(packet,count)==1 && count==20 && KeyboardProtocol::WirelessChangeHint(packet.data(),count));
        input.Pause();assert(input.Event()==stableEvent && cancels==2 && drains==2);
        assert(input.Consume(packet,count)==0);assert(input.Resume());
        input.Pause();assert(input.Event()==stableEvent && cancels==3 && drains==3);
        input.Close();assert(!input.Event());
    }
    reads=0;shortReport=true;
    {KeyboardNotifications::RadioInput input;assert(input.Configure(L"isolated-radio"));SetEvent(input.Event());
     std::array<BYTE,20> packet{};assert(input.Consume(packet,count)==-1);}
    shortReport=false;
    KeyboardNotifications::Policy policy;policy.Reset(0);
    const BYTE radio[]={0x13,0x0A,1,0,4,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0x29};
    assert(!policy.RadioHint(radio,20,100) && policy.RadioHint(radio,20,500));
    assert(policy.Ready(1500));policy.Read(true,true,true,1500);
    assert(!policy.RadioHint(radio,19,2000));
    const BYTE connection[]={0x13,0x0A,1,0,4,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0x25};
    assert(policy.RadioHint(connection,20,2000) && !policy.Ready(2999) && policy.Ready(3000));
    puts("PASS: isolated vendor-only read access, wrong endpoint rejection, overlapped cancellation/drain, synchronous completion, short reports and rearm failures; no real HID");
}
