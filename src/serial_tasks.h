#pragma once
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <system_error>

// One bounded queue per device. Replacement only affects work not yet started.
// Device managers retain ownership of their transport locks and validation.
class SerialTasks {
    struct Job { unsigned key; std::function<void()> run; };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::thread thread;
    HANDLE ioThread=nullptr;
    bool ready=false;
    bool stopping=true;
public:
    ~SerialTasks(){Stop();}
    bool Start() {
        std::unique_lock<std::mutex> lock(mutex);
        if(thread.joinable())return !stopping;
        stopping=false;ready=false;
        try {thread=std::thread([this]{
            {std::lock_guard<std::mutex> lock(mutex);
             if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&ioThread,
                                 THREAD_TERMINATE,FALSE,0))stopping=true;
             ready=true;changed.notify_all();}
            for(;;) {
                Job job;
                {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[this]{return stopping || !jobs.empty();});
                 if(stopping)return;
                 job=std::move(jobs.front());jobs.pop_front();}
                job.run();
            }
        });}catch(const std::system_error&){stopping=true;return false;}
        changed.wait(lock,[this]{return ready;});
        if(stopping){lock.unlock();Stop();return false;}
        return true;
    }
    bool Submit(unsigned key,std::function<void()> run) {
        std::lock_guard<std::mutex> lock(mutex);
        if(stopping)return false;
        for(auto& job:jobs)if(job.key==key){job.run=std::move(run);return true;}
        if(jobs.size()>=8)return false;
        jobs.push_back({key,std::move(run)});changed.notify_one();return true;
    }
    void Stop() {
        HANDLE cancelThread;
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;jobs.clear();cancelThread=ioThread;}
        changed.notify_all();
        if(cancelThread)CancelSynchronousIo(cancelThread);
        if(thread.joinable())thread.join();
        {std::lock_guard<std::mutex> lock(mutex);if(ioThread){CloseHandle(ioThread);ioThread=nullptr;}}
    }
};
