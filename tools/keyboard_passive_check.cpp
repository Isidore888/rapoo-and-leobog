// Default: bounded passive observation of vendor HID input collections, no requests.
// Opt-in --snapshot/--notify-config: identity/config queries only (82/84), never
// settings writes or standard keyboard collection reads. Stop queries on failure.
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <cstdio>
#include <thread>
#include <mutex>
#include <vector>
#include <cstring>
static std::mutex output;
static HANDLE configHandle=INVALID_HANDLE_VALUE;
static bool configQueriesEnabled=true;
static bool FeatureQuery(BYTE command,BYTE param,BYTE length,BYTE* reply) {
    KeyboardProtocol::WiredRequest(command,param,length,reply);
    if(!HidD_SetFeature(configHandle,reply,520)){printf("query command=%02X set_feature_failed error=%lu\n",command,GetLastError());return false;}
    const auto deadline=GetTickCount64()+150;
    do {
        Sleep(25);std::fill(reply,reply+520,0);reply[0]=6;
        if(HidD_GetFeature(configHandle,reply,520) && KeyboardProtocol::WiredHeader(reply,520,command,param,length))return true;
    }while(GetTickCount64()<deadline);
    printf("query command=%02X response_not_validated last_header=",command);
    for(int i=0;i<16;++i)printf("%02X%s",reply[i],i==15?"\n":" ");
    return false;
}
static bool ReadConfiguration(const char* reason) {
    BYTE reply[520]{};KeyboardProtocol::Lighting light;
    if(!FeatureQuery(0x84,0,128,reply)) {
        printf("config reason=%s read_failed; further configuration queries disabled\n",reason);fflush(stdout);return false;
    }
    printf("config_page reason=%s bytes=",reason);
    for(int i=0;i<128;++i)printf("%02X%s",reply[i+8],i==127?"\n":" ");
    fflush(stdout);
    if(!KeyboardProtocol::ParseLighting(reply+8,128,light)) {
        printf("config reason=%s transport_valid=1 page_valid=%d lighting_parse_valid=0\n",reason,KeyboardProtocol::ConfigPage(reply+8,128));return false;
    }
    printf("config reason=%s tick=%llu effect=%d brightness=%d speed=%d\n",reason,GetTickCount64(),light.effect,light.brightness,light.speed);fflush(stdout);
    return true;
}
static bool Target(const Hid::Endpoint& ep) {
    return ((ep.vid==0x258A && ep.pid==0x010C) || (ep.vid==0x3554 && ep.pid==0xFA09)) && ep.caps.UsagePage>=0xFF00;
}
static void Observe(Hid::Endpoint ep, unsigned id, ULONGLONG until) {
    HANDLE h=CreateFileW(ep.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(h==INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(output);printf("open_failed endpoint=%u error=%lu\n",id,GetLastError());return;
    }
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event){CloseHandle(h);return;}
    std::vector<BYTE> data(ep.caps.InputReportByteLength);
    unsigned reports=0;
    {std::lock_guard<std::mutex> lock(output);printf("listening endpoint=%u\n",id);fflush(stdout);}
    while(GetTickCount64()<until) {
        OVERLAPPED ov{};ov.hEvent=event;ResetEvent(event);DWORD n=0;
        BOOL ok=ReadFile(h,data.data(),DWORD(data.size()),&n,&ov);
        if(!ok && GetLastError()==ERROR_IO_PENDING) {
            const auto now=GetTickCount64();DWORD wait=now<until?DWORD(until-now):0;
            if(WaitForSingleObject(event,wait)==WAIT_OBJECT_0)ok=GetOverlappedResult(h,&ov,&n,FALSE);
            else {CancelIoEx(h,&ov);GetOverlappedResult(h,&ov,&n,TRUE);break;}
        }
        if(!ok) {std::lock_guard<std::mutex> lock(output);printf("read_failed endpoint=%u error=%lu\n",id,GetLastError());break;}
        if(n) {
            ++reports;std::lock_guard<std::mutex> lock(output);
            printf("report endpoint=%u tick=%llu length=%lu bytes=",id,GetTickCount64(),n);
            for(DWORD i=0;i<n;++i)printf("%02X%s",data[i],i+1==n?"\n":" ");
            fflush(stdout);
            if(configHandle!=INVALID_HANDLE_VALUE && configQueriesEnabled && ep.vid==0x258A && data.size()==4 && n==4 && data[0]==3 && data[1]==8 && data[2]==0 && data[3]==1) {
                configQueriesEnabled=ReadConfiguration("notification");
                if(configQueriesEnabled){Sleep(500);configQueriesEnabled=ReadConfiguration("after-500ms");}
                if(configQueriesEnabled){Sleep(1000);configQueriesEnabled=ReadConfiguration("after-1500ms");}
                // Notifications are not proven exclusive to physical shortcuts.
                // Bound successful queries as well to prevent a request/notification loop.
                if(reports>=4)configQueriesEnabled=false;
            }
            if(reports>=4096) {puts("report_limit_reached");break;}
        }
    }
    CloseHandle(event);CloseHandle(h);
    std::lock_guard<std::mutex> lock(output);printf("finished endpoint=%u reports=%u\n",id,reports);fflush(stdout);
}
int main(int argc,char** argv) {
    const bool listOnly=argc==2 && strcmp(argv[1],"--list")==0;
    const bool notifyConfig=argc==2 && strcmp(argv[1],"--notify-config")==0;
    const bool snapshot=argc==2 && strcmp(argv[1],"--snapshot")==0;
    const bool shortPassive=argc==2 && strcmp(argv[1],"--passive-45")==0;
    if(!listOnly && !notifyConfig && !snapshot && !shortPassive && argc!=1)return 2;
    std::vector<Hid::Endpoint> endpoints;
    for(const auto& ep:Hid::Enumerate())if(Target(ep))endpoints.push_back(ep);
    for(unsigned i=0;i<endpoints.size();++i) {
        const auto& ep=endpoints[i];printf("endpoint=%u vid=%04X pid=%04X usage_page=%04X usage=%04X input=%u output=%u feature=%u\n",i,ep.vid,ep.pid,ep.caps.UsagePage,ep.caps.Usage,ep.caps.InputReportByteLength,ep.caps.OutputReportByteLength,ep.caps.FeatureReportByteLength);
    }
    if(listOnly)return 0;
    if(notifyConfig || snapshot) {
        for(const auto& ep:endpoints)if(ep.vid==0x258A && ep.caps.FeatureReportByteLength==520) {
            configHandle=Hid::Open(ep,false);break;
        }
        if(configHandle==INVALID_HANDLE_VALUE)return 3;
        BYTE identity[520]{};
        if(!FeatureQuery(0x82,1,6,identity) || !KeyboardProtocol::WiredIdentity(identity,520)){CloseHandle(configHandle);return 4;}
        const bool valid=ReadConfiguration("baseline");
        if(snapshot || !valid){CloseHandle(configHandle);return valid?0:5;}
    }
    const unsigned seconds=shortPassive?45:90;
    printf("passive_start tick=%llu duration_seconds=%u mode=%s\n",GetTickCount64(),seconds,notifyConfig?"notification-triggered-config-queries":"zero-requests");fflush(stdout);
    ULONGLONG until=GetTickCount64()+seconds*1000;std::vector<std::thread> threads;
    for(unsigned i=0;i<endpoints.size();++i)if(endpoints[i].caps.InputReportByteLength)threads.emplace_back(Observe,endpoints[i],i,until);
    for(auto& thread:threads)thread.join();
    if(configHandle!=INVALID_HANDLE_VALUE)CloseHandle(configHandle);
    puts("observation_complete setting_writes=0");return 0;
}
