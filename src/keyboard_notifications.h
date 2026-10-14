#pragma once
#include "hid_transport.h"
#include "keyboard_protocol.h"
#include <algorithm>
#include <array>

namespace KeyboardNotifications {
// Observed USB4, radio07 and radio02 events trigger verified state queries.
// Bound queries even if reads cause notifications. A useless/failed hint read
// disables hints until the connection changes; ordinary polling remains intact.
struct Policy {
    bool enabled=true,pending=false;
    ULONGLONG first=0,due=0,window=0,ignoreBefore=0;
    unsigned queries=0,reports=0;
    void Reset(ULONGLONG now) {*this=Policy{};window=now;ignoreBefore=now+250;}
    void Disable() {enabled=false;pending=false;}
    bool Hint(const BYTE* data,DWORD size,ULONGLONG now) {
        if(!enabled)return false;
        if(now-window>=60000){window=now;queries=reports=0;}
        if(++reports>256){Disable();return false;}
        if(size!=4 || data[0]!=3 || data[1]!=8 || data[2]!=0 || data[3]!=1)return false;
        if(now<ignoreBefore || queries>=4)return false;
        if(!pending){first=now;pending=true;}
        due=std::min(now+1000,first+2000);return true;
    }
    bool RadioHint(const BYTE* data,DWORD size,ULONGLONG now) {
        const BYTE hint[]={3,8,0,1};
        return (KeyboardProtocol::WirelessChangeHint(data,size) || KeyboardProtocol::WirelessConnectionHint(data,size))?
            Hint(hint,4,now):Hint(nullptr,0,now);
    }
    ULONGLONG NextDue(ULONGLONG normal) const {return enabled && pending?std::min(normal,due):normal;}
    bool Ready(ULONGLONG now) const {return enabled && pending && now>=due;}
    void Read(bool valid,bool changed,bool hinted,ULONGLONG now) {
        ignoreBefore=now+250;
        if(hinted){++queries;pending=false;if(!valid || !changed)Disable();}
        else if(changed)pending=false;
        if(!valid)Disable();
    }
};

// Radio notifications and query replies share one input endpoint. Every query
// or setting transaction pauses/drains this reader under the manager's ioMutex.
// Keep the event alive while paused: the worker may already be waiting on it.
class RadioInput {
    HANDLE handle=INVALID_HANDLE_VALUE,event=nullptr;
    OVERLAPPED io{};
    std::array<BYTE,20> buffer{};
    std::wstring path;
    DWORD immediate=0;
    bool inFlight=false;
    bool Arm() {
        ResetEvent(event);io={};io.hEvent=event;immediate=0;
        if(ReadFile(handle,buffer.data(),DWORD(buffer.size()),&immediate,&io)) {
            inFlight=false;SetEvent(event);return true;
        }
        inFlight=GetLastError()==ERROR_IO_PENDING;return inFlight;
    }
public:
    RadioInput()=default;
    RadioInput(const RadioInput&)=delete;
    RadioInput& operator=(const RadioInput&)=delete;
    ~RadioInput(){Close();}
    void Pause() {
        if(handle!=INVALID_HANDLE_VALUE) {
            if(inFlight){CancelIoEx(handle,&io);DWORD count=0;GetOverlappedResult(handle,&io,&count,TRUE);}
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
        }
        inFlight=false;if(event)ResetEvent(event);
    }
    bool Resume() {
        if(handle!=INVALID_HANDLE_VALUE)return true;
        if(path.empty() || !event)return false;
        handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(handle!=INVALID_HANDLE_VALUE && Arm())return true;
        Pause();return false;
    }
    bool Configure(const std::wstring& verifiedPath) {
        Pause();path=verifiedPath;
        if(!event)event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        return Resume();
    }
    HANDLE Event() const{return event;}
    // 0: stale wake or paused reader, 1: completed report, -1: read failure.
    int Consume(std::array<BYTE,20>& report,DWORD& count) {
        count=0;
        if(handle==INVALID_HANDLE_VALUE || WaitForSingleObject(event,0)!=WAIT_OBJECT_0)return 0;
        count=immediate;
        if(inFlight && !GetOverlappedResult(handle,&io,&count,FALSE)){Pause();return -1;}
        inFlight=false;
        if(count!=20){Pause();return -1;}
        report=buffer;
        if(!Arm()){Pause();return -1;}
        return 1;
    }
    void Close() {Pause();path.clear();if(event){CloseHandle(event);event=nullptr;}}
};

class Input {
    HANDLE handle=INVALID_HANDLE_VALUE,event=nullptr;
    OVERLAPPED io{};
    std::array<BYTE,4> buffer{};
    DWORD immediate=0;
    bool inFlight=false;
    bool Arm() {
        ResetEvent(event);io={};io.hEvent=event;immediate=0;
        if(ReadFile(handle,buffer.data(),DWORD(buffer.size()),&immediate,&io)) {
            inFlight=false;SetEvent(event);return true;
        }
        inFlight=GetLastError()==ERROR_IO_PENDING;return inFlight;
    }
public:
    Input()=default;
    Input(const Input&)=delete;
    Input& operator=(const Input&)=delete;
    ~Input(){Close();}
    void Close() {
        if(handle!=INVALID_HANDLE_VALUE) {
            if(inFlight){CancelIoEx(handle,&io);DWORD count=0;GetOverlappedResult(handle,&io,&count,TRUE);}
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
        }
        if(event){CloseHandle(event);event=nullptr;}
        inFlight=false;
    }
    bool Attach() {
        Close();
        for(const auto& ep:Hid::Enumerate()) {
            const auto& c=ep.caps;
            if(ep.vid!=0x258A || ep.pid!=0x010C || c.UsagePage!=0xFF00 || c.Usage!=1 || c.InputReportByteLength!=4)continue;
            handle=CreateFileW(ep.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(handle==INVALID_HANDLE_VALUE)return false;
            event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(event && Arm())return true;
            Close();return false;
        }
        return false;
    }
    HANDLE Event() const {return event;}
    bool Consume(std::array<BYTE,4>& report,DWORD& count) {
        count=immediate;
        if(inFlight && !GetOverlappedResult(handle,&io,&count,FALSE)){Close();return false;}
        inFlight=false;
        if(count!=4){Close();return false;}
        report=buffer;
        if(!Arm()){Close();return false;}
        return true;
    }
};
}
