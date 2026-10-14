// Bounded USB correlation experiment. Read vendor input4 only; manually triggered
// identity/config queries never settings writes. Opt-in notification modes issue
// only one delayed config query, then observe a bounded tail without queries.
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

static std::mutex logMutex;
static std::atomic<bool> stopped{false};
static std::atomic<unsigned> reports{0};
static std::atomic<bool> failed{false};
static bool notifyOnce=false;
static DWORD settleMs=150;
static HANDLE notificationConfig=INVALID_HANDLE_VALUE;
static std::atomic<bool> queriedNotification{false};
static std::atomic<ULONGLONG> notificationDeadline{0};
static bool Exchange(HANDLE h,BYTE command,BYTE parameter,BYTE length,BYTE* reply) {
    KeyboardProtocol::WiredRequest(command,parameter,length,reply);
    if(!HidD_SetFeature(h,reply,520))return false;
    const auto until=GetTickCount64()+150;
    do {
        Sleep(25);std::fill(reply,reply+520,0);reply[0]=6;
        if(HidD_GetFeature(h,reply,520) && KeyboardProtocol::WiredHeader(reply,520,command,parameter,length))return true;
    }while(GetTickCount64()<until);
    return false;
}
static bool Snapshot(HANDLE h,const char* phase) {
    BYTE reply[520]{};KeyboardProtocol::Lighting light;int sleep=0;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        printf("query_begin phase=%s tick=%llu reports=%u\n",phase,GetTickCount64(),reports.load());fflush(stdout);
    }
    const bool transport=Exchange(h,0x84,0,128,reply);
    const bool valid=transport && KeyboardProtocol::ParseLighting(reply+8,128,light) && KeyboardProtocol::ParseSleep(reply+8,128,sleep);
    std::lock_guard<std::mutex> lock(logMutex);
    printf("query_end phase=%s tick=%llu valid=%d transport=%d reports=%u effect=%d brightness=%d speed=%d sleep_seconds=%d\n",
           phase,GetTickCount64(),valid,transport,reports.load(),light.effect,light.brightness,light.speed,sleep);
    printf("page phase=%s bytes=",phase);
    for(int i=0;i<128;++i)printf("%02X%s",reply[i+8],i==127?"\n":" ");
    fflush(stdout);return valid;
}
static void Listen(HANDLE h,ULONGLONG until) {
    while(!stopped && GetTickCount64()<until) {
        BYTE data[4]{};DWORD count=0;
        // Short timeout is expected when idle, not a device failure. Transfer
        // drains cancelled OVERLAPPED before releasing its buffer.
        if(!Hid::Transfer(h,data,4,false,200,&count)) {
            const auto error=GetLastError();
            if(error==ERROR_OPERATION_ABORTED || error==ERROR_IO_PENDING || error==ERROR_SUCCESS)continue;
            std::lock_guard<std::mutex> lock(logMutex);
            printf("input_failed error=%lu stop=1\n",error);fflush(stdout);failed=true;stopped=true;break;
        }
        const auto number=++reports;
        {
            std::lock_guard<std::mutex> lock(logMutex);
            printf("report number=%u tick=%llu length=%lu bytes=",number,GetTickCount64(),count);
            for(DWORD i=0;i<count && i<4;++i)printf("%02X%s",data[i],i+1==count?"\n":" ");
            fflush(stdout);
        }
        if(count!=4 || number>=256){puts("invalid_length_or_report_limit stop=1");failed=true;stopped=true;}
        if(!stopped && notifyOnce && count==4 && data[0]==3 && data[1]==8 && data[2]==0 && data[3]==1 && !queriedNotification.exchange(true)) {
            Sleep(settleMs);
            if(!Snapshot(notificationConfig,"notification-once")){failed=true;stopped=true;}
            notificationDeadline=GetTickCount64()+10000;
        }
    }
}
int main(int argc,char** argv) {
    notifyOnce=argc==2 && !strcmp(argv[1],"--notify-once");
    if(argc==2 && !strcmp(argv[1],"--notify-once-1000")){notifyOnce=true;settleMs=1000;}
    if(argc!=1 && !notifyOnce)return 2;
    HANDLE config=INVALID_HANDLE_VALUE,input=INVALID_HANDLE_VALUE;
    for(const auto& ep:Hid::Enumerate()) {
        if(ep.vid!=0x258A || ep.pid!=0x010C || ep.caps.UsagePage!=0xFF00 || ep.caps.Usage!=1)continue;
        if(ep.caps.FeatureReportByteLength==520 && config==INVALID_HANDLE_VALUE)config=Hid::Open(ep,false);
        if(ep.caps.InputReportByteLength==4 && input==INVALID_HANDLE_VALUE)
            input=CreateFileW(ep.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    }
    if(config==INVALID_HANDLE_VALUE || input==INVALID_HANDLE_VALUE) {
        if(config!=INVALID_HANDLE_VALUE)CloseHandle(config);
        if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
        puts("required_usb_endpoints_missing stop=1");return 2;
    }
    BYTE identity[520]{};
    if(!Exchange(config,0x82,1,6,identity) || !KeyboardProtocol::WiredIdentity(identity,520) || !Snapshot(config,"baseline")) {
        CloseHandle(input);CloseHandle(config);puts("baseline_failed no_listening=1 stop=1");return 3;
    }
    const auto until=GetTickCount64()+300000;
    notificationConfig=config;
    std::thread reader(Listen,input,until);
    printf("controlled_start tick=%llu seconds=300 configuration_budget=%u settle_ms=%lu input4_only=1 setting_writes=0\n",GetTickCount64(),notifyOnce?1u:6u,settleMs);fflush(stdout);
    unsigned queries=0;std::string previous;
    while(!stopped && GetTickCount64()<until) {
        if(notifyOnce && notificationDeadline && GetTickCount64()>=notificationDeadline)break;
        std::string value;
        {std::ifstream command("verification/optimization-next/control.txt");std::getline(command,value);}
        if(!value.empty() && value!=previous) {
            previous=value;
            if(value=="done"){stopped=true;break;}
            if(notifyOnce){puts("manual_query_not_allowed stop=1");failed=true;stopped=true;break;}
            const bool recognized=value=="query-only" || value=="brightness-down" || value=="brightness-restored" || value=="speed-down" || value=="speed-restored" || value=="arrows-only";
            if(!recognized || queries>=6) {
                puts("invalid_command_or_budget stop=1");failed=true;stopped=true;break;
            }
            ++queries;
            if(!Snapshot(config,value.c_str())){puts("first_query_failure stop=1");failed=true;stopped=true;break;}
        }
        Sleep(100);
    }
    stopped=true;reader.join();CloseHandle(input);CloseHandle(config);
    printf("controlled_complete queries=%u notification_queries=%d reports=%u setting_writes=0\n",queries,queriedNotification.load(),reports.load());fflush(stdout);
    return failed?4:0;
}
