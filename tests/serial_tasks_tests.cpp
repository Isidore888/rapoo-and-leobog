#include <windows.h>
#include <atomic>
static std::atomic<bool> failThreadHandle{false};
static std::atomic<int> cancellations{0};
static BOOL WINAPI TestDuplicate(HANDLE from,HANDLE handle,HANDLE to,LPHANDLE out,DWORD access,BOOL inherit,DWORD flags) {
    if(failThreadHandle){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return DuplicateHandle(from,handle,to,out,access,inherit,flags);
}
static BOOL WINAPI TestCancel(HANDLE thread){++cancellations;return CancelSynchronousIo(thread);}
#define DuplicateHandle TestDuplicate
#define CancelSynchronousIo TestCancel
#include "../src/serial_tasks.h"
#undef DuplicateHandle
#undef CancelSynchronousIo
#include <cassert>
#include <chrono>
#include <future>
#include <vector>
#include <cstdio>
using namespace std::chrono_literals;
int main() {
    SerialTasks mouse,keyboard;assert(mouse.Start() && keyboard.Start());
    std::promise<void> entered,release,mouseDone,keyboardDone;
    auto unblock=release.get_future().share();std::vector<int> order;
    assert(keyboard.Submit(1,[&]{entered.set_value();unblock.wait();order.push_back(1);}));
    assert(entered.get_future().wait_for(2s)==std::future_status::ready);
    assert(keyboard.Submit(1,[&]{order.push_back(2);}));
    assert(keyboard.Submit(1,[&]{order.push_back(3);})); // Pending replacement, never active replacement.
    assert(keyboard.Submit(2,[&]{order.push_back(4);keyboardDone.set_value();}));
    assert(mouse.Submit(1,[&]{mouseDone.set_value();}));
    assert(mouseDone.get_future().wait_for(2s)==std::future_status::ready); // Separate device remains responsive.
    release.set_value();assert(keyboardDone.get_future().wait_for(2s)==std::future_status::ready);
    mouse.Stop();keyboard.Stop();assert(order==std::vector<int>({1,3,4}));
    assert(!keyboard.Submit(1,[]{}));
    assert(keyboard.Start());std::promise<void> running,finish;auto finishWait=finish.get_future().share();
    assert(keyboard.Submit(0,[&]{running.set_value();finishWait.wait();}));
    assert(running.get_future().wait_for(2s)==std::future_status::ready);
    for(unsigned i=1;i<=8;++i)assert(keyboard.Submit(i,[]{assert(false && "pending job must be cancelled on exit");}));
    assert(!keyboard.Submit(9,[]{}));
    auto stopping=std::async(std::launch::async,[&]{keyboard.Stop();});
    // Wait until Stop has closed submissions, then let the active job finish.
    while(keyboard.Submit(1,[]{assert(false);}))std::this_thread::yield();
    finish.set_value();assert(stopping.wait_for(2s)==std::future_status::ready);
    assert(cancellations==3);
    failThreadHandle=true;assert(!keyboard.Start() && !keyboard.Submit(1,[]{}));keyboard.Stop();
    failThreadHandle=false;assert(keyboard.Start());keyboard.Stop();
    puts("PASS: independent device queues, serial execution, pending-only replacement, bounded backlog and shutdown cancellation");
}
