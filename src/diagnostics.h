#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cwchar>
#include <string>

namespace Diagnostics {
inline std::mutex mutex;
inline HANDLE file=INVALID_HANDLE_VALUE;
inline DWORD bytes=0,limit=8*1024*1024;
inline unsigned long long sequence=0;
inline std::wstring directory;
inline unsigned long long nextPart=0;
inline std::atomic<unsigned long long> ids{0},activeUsbQuery{0},activeUsbIo{0};
inline thread_local unsigned long long query=0;
inline thread_local BYTE command=0,parameter=0;
struct PreserveError {DWORD saved=GetLastError();~PreserveError(){SetLastError(saved);}};
// Caller holds mutex; CREATE_NEW cannot overwrite an earlier segment.
inline bool OpenPartLocked() {
    SYSTEMTIME utc;GetSystemTime(&utc);
    for(unsigned attempt=0;attempt<100;++attempt) {
        wchar_t name[120];std::swprintf(name,120,L"/usb-%04u%02u%02uT%02u%02u%02u-%lu-%llu.jsonl",
            utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,GetCurrentProcessId(),nextPart++);
        file=CreateFileW((directory+name).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){bytes=0;return true;}
        if(GetLastError()!=ERROR_FILE_EXISTS)return false;
    }
    return false;
}
inline bool WriteLocked(const char* data,DWORD size) {
    DWORD done=0;
    if(!WriteFile(file,data,size,&done,nullptr) || done!=size) {
        CloseHandle(file);file=INVALID_HANDLE_VALUE;return false;
    }
    bytes+=done;return true;
}
// Only fixed internal labels and numeric metadata belong here; never device payloads or paths.
inline void Log(const char* event,unsigned long long op=0,unsigned cmd=0,unsigned param=0,
                unsigned result=0,DWORD error=0,unsigned long long detail=0,ULONGLONG duration=0) {
    PreserveError preserve;
    std::lock_guard<std::mutex> lock(mutex);
    if(file==INVALID_HANDLE_VALUE)return;
    SYSTEMTIME utc;GetSystemTime(&utc);
    char line[600];int n=std::snprintf(line,sizeof(line),
        "{\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\"tick_ms\":%llu,\"seq\":%llu,\"pid\":%lu,\"tid\":%lu,\"event\":\"%s\",\"op\":%llu,\"query\":%llu,\"cmd\":%u,\"param\":%u,\"result\":%u,\"error\":%lu,\"detail\":%llu,\"duration_ms\":%llu,\"active_usb_query\":%llu,\"active_usb_io\":%llu}\n",
        utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds,
        GetTickCount64(),++sequence,GetCurrentProcessId(),GetCurrentThreadId(),event,op,query,cmd,param,
        result,error,detail,duration,activeUsbQuery.load(),activeUsbIo.load());
    if(n<=0 || n>=int(sizeof(line)))return;
    if(bytes+DWORD(n)+64>limit) {
        const char end[]="{\"event\":\"log_segment_end\",\"continues\":true}\n";
        if(!WriteLocked(end,sizeof(end)-1))return;
        CloseHandle(file);file=INVALID_HANDLE_VALUE;
        if(!OpenPartLocked())return;
        const char begin[]="{\"event\":\"log_segment_start\",\"continues\":true}\n";
        if(!WriteLocked(begin,sizeof(begin)-1))return;
    }
    WriteLocked(line,DWORD(n));
}
inline bool Init(const std::wstring& targetDirectory,DWORD maxBytes=8*1024*1024) {
    PreserveError preserve;
    {std::lock_guard<std::mutex> lock(mutex);
        if(file!=INVALID_HANDLE_VALUE)return true;
        if(maxBytes<1024)return false;
        if(!CreateDirectoryW(targetDirectory.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)return false;
        directory=targetDirectory;nextPart=0;limit=maxBytes;sequence=0;
        if(!OpenPartLocked())return false;
    }
    Log("session_start");
    Log("usb_profile_original",0,0,0,1,0,3000);
    return true;
}
inline bool Init() {
    PreserveError preserve;
    wchar_t exe[32768];DWORD n=GetModuleFileNameW(nullptr,exe,32768);
    if(!n || n>=32768)return false;
    std::wstring directory(exe,n);auto slash=directory.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return false;
    return Init(directory.substr(0,slash)+L"/logs");
}
inline void Close() {
    PreserveError preserve;Log("session_end");
    std::lock_guard<std::mutex> lock(mutex);
    if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
}
struct UsbQuery {
    const unsigned long long id=++ids,previousQuery=query;
    const BYTE previousCommand=command,previousParameter=parameter;
    const ULONGLONG started=GetTickCount64();
    unsigned result=0;DWORD error=0;
    UsbQuery(BYTE cmd,BYTE param,BYTE len) {
        query=id;command=cmd;parameter=param;activeUsbQuery=id;
        Log("query_begin",id,cmd,param,0,0,len);
    }
    ~UsbQuery() {
        Log("query_end",id,command,parameter,result,error,0,GetTickCount64()-started);
        activeUsbQuery=previousQuery;query=previousQuery;command=previousCommand;parameter=previousParameter;
    }
};
}
