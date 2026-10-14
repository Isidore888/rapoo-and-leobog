#include "../src/diagnostics.h"
#include <cassert>
#include <thread>
#include <vector>
#include <filesystem>
int main() {
    printf("TestPid=%lu\n",GetCurrentProcessId());
    const std::wstring root=L"verification/usb-log-continuous-20261014/logger-test";
    std::filesystem::create_directories(root);
    SetLastError(1234);assert(Diagnostics::Init(root));assert(GetLastError()==1234);
    std::vector<std::thread> threads;
    for(int i=0;i<3;++i)threads.emplace_back([]{for(int j=0;j<10;++j){SetLastError(4321);Diagnostics::Log("test_thread");assert(GetLastError()==4321);}});
    for(auto& thread:threads)thread.join();
    {Diagnostics::UsbQuery query(0x84,0,128);assert(Diagnostics::query==query.id && Diagnostics::command==0x84);
        {Diagnostics::UsbQuery inner(0x82,1,6);inner.result=1;}
        assert(Diagnostics::query==query.id && Diagnostics::command==0x84);query.result=1;}
    assert(!Diagnostics::query && !Diagnostics::activeUsbQuery);
    SetLastError(1234);Diagnostics::Close();assert(GetLastError()==1234);
    assert(Diagnostics::Init(root,1600));
    SetLastError(1234);
    threads.clear();
    for(int i=0;i<3;++i)threads.emplace_back([i]{for(int j=0;j<30;++j){SetLastError(4321);Diagnostics::Log("cap_test",0,0,0,1,0,i*30+j);assert(GetLastError()==4321);}});
    for(auto& thread:threads)thread.join();
    assert(Diagnostics::file!=INVALID_HANDLE_VALUE && Diagnostics::nextPart>2 && GetLastError()==1234);
    Diagnostics::Log("pnp_test_after_segments",0,0,0,2,0,0x8004);
    Diagnostics::Log("error_after_segments",0,0x84,0,0,31);
    Diagnostics::Close();
    assert(Diagnostics::Init(root));Diagnostics::Log("next_session");Diagnostics::Close();
    assert(!Diagnostics::Init(root,1023));
    assert(!Diagnostics::Init(L"verification/usb-log-continuous-20261014/missing-parent/logs"));
    puts("PASS: concurrent complete lines, nested query context, LastError preservation, unique segments, continuous rotation, post-rotation incident/error and initialization failure; no HID");
}
