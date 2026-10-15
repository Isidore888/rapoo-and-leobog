#include <windows.h>
static int writeFault=0;
static BOOL WINAPI TestWrite(HANDLE file,LPCVOID data,DWORD size,LPDWORD done,LPOVERLAPPED overlapped) {
    if(writeFault==1){SetLastError(ERROR_DISK_FULL);return FALSE;}
    if(writeFault==2){*done=0;return TRUE;}
    return ::WriteFile(file,data,size,done,overlapped);
}
#define WriteFile TestWrite
#include "../src/diagnostics.h"
#undef WriteFile
#include <cassert>
#include <thread>
#include <vector>
#include <filesystem>
int main() {
    printf("TestPid=%lu\n",GetCurrentProcessId());
    const std::wstring root=L"verification/diagnostics-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
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
    for(int i=0;i<3;++i)threads.emplace_back([i]{for(int j=0;j<60;++j){SetLastError(4321);Diagnostics::Log("cap_test",0,0,0,1,0,i*60+j);assert(GetLastError()==4321);}});
    for(auto& thread:threads)thread.join();
    assert(Diagnostics::file!=INVALID_HANDLE_VALUE && Diagnostics::nextPart>Diagnostics::maxSegments && GetLastError()==1234);
    Diagnostics::Log("pnp_test_after_segments",0,0,0,2,0,0x8004);
    Diagnostics::Log("error_after_segments",0,0x84,0,0,31);
    Diagnostics::Close();
    std::size_t retained=0;
    for(const auto& entry:std::filesystem::directory_iterator(root))if(entry.is_regular_file())++retained;
    assert(retained<=Diagnostics::maxSegments);
    assert(Diagnostics::Init(root));Diagnostics::Log("next_session");Diagnostics::Close();
    assert(!Diagnostics::Init(root,1023));
    assert(!Diagnostics::Init(root+L"/missing-parent/logs"));
    assert(Diagnostics::Init(root));
    writeFault=1;SetLastError(1234);Diagnostics::Log("disk_full");
    assert(GetLastError()==1234 && Diagnostics::file==INVALID_HANDLE_VALUE);
    assert(Diagnostics::TakeWriteFailure()==ERROR_DISK_FULL && !Diagnostics::TakeWriteFailure());
    Diagnostics::Log("still_stopped");assert(!Diagnostics::TakeWriteFailure());
    writeFault=0;assert(Diagnostics::Init(root));
    writeFault=2;Diagnostics::Log("short_write");
    assert(Diagnostics::TakeWriteFailure()==ERROR_WRITE_FAULT && !Diagnostics::TakeWriteFailure());
    writeFault=0;assert(Diagnostics::Init(root,1600));
    Diagnostics::directory=root+L"/missing-parent";Diagnostics::bytes=1600;
    Diagnostics::Log("rotation_open_failed");
    assert(Diagnostics::file==INVALID_HANDLE_VALUE && Diagnostics::TakeWriteFailure()!=ERROR_SUCCESS);
    assert(!Diagnostics::TakeWriteFailure());
    assert(Diagnostics::Init(root));Diagnostics::Close();assert(!Diagnostics::TakeWriteFailure());
    puts("PASS: disk-full, short write and segment creation failure signaled once; normal close is not a failure; LastError preserved");
    puts("PASS: concurrent complete lines, nested query context, LastError preservation, unique segments, continuous rotation with bounded retention, post-rotation incident/error and initialization failure; no HID");
}
