// Radio state-packet correlation: one known read-only baseline, passive input,
// at most one explicitly opted-in post-event snapshot. No configuration writes.
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <cstdio>
#include <cstring>
static const char* phase="baseline-query";
namespace Hid {
static bool radioOnlyQueries=false;
static std::vector<Endpoint> StateEnumerate() {
    auto endpoints=Enumerate();
    // This diagnostic's new snapshot authorization covers radio only.
    if(radioOnlyQueries)endpoints.erase(std::remove_if(endpoints.begin(),endpoints.end(),[](const Endpoint& ep) {
        return ep.vid!=0x3554 || ep.pid!=0xFA09 || ep.caps.UsagePage!=0xFF02 || ep.caps.Usage!=2 ||
            ep.caps.InputReportByteLength!=20 || ep.caps.OutputReportByteLength!=20;
    }),endpoints.end());
    return endpoints;
}
static bool StateTransfer(HANDLE h,BYTE* data,DWORD size,bool write,DWORD timeout,DWORD* received=nullptr) {
    DWORD count=0;bool ok=Transfer(h,data,size,write,timeout,&count);
    printf("transport phase=%s direction=%s ok=%d count=%lu bytes=",phase,write?"query":"reply",ok,count);
    for(DWORD i=0;i<(write?size:count) && i<size;++i)printf("%02X ",data[i]);
    puts("");fflush(stdout);if(received)*received=count;return ok;
}
}
#define Transfer StateTransfer
#define Enumerate StateEnumerate
#include "../src/keyboard_manager.cpp"
#undef Enumerate
#undef Transfer
static bool Snapshot(const char* label,Keyboard::State& state) {
    phase=label;state=Keyboard::Query();
    const bool valid=state.isConnected && !state.isWired && state.batteryValid && state.lightingValid && state.sleepValid;
    printf("snapshot phase=%s valid=%d tick=%llu battery=%d effect=%d brightness=%d speed=%d sleep=%d\n",
        phase,valid,GetTickCount64(),state.battery,state.lightEffect,state.brightness,state.lightSpeed,state.sleepSeconds);
    fflush(stdout);return valid;
}
static HANDLE Input() {
    for(const auto& ep:Hid::Enumerate())if(ep.vid==0x3554 && ep.pid==0xFA09 && ep.caps.UsagePage==0xFF02 && ep.caps.Usage==2 &&
        ep.caps.InputReportByteLength==20 && ep.caps.OutputReportByteLength==20)
        return CreateFileW(ep.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    return INVALID_HANDLE_VALUE;
}
int main(int argc,char** argv) {
    const bool initPassive=argc==2 && !strcmp(argv[1],"--init-passive");
    const bool one=argc==2 && !strcmp(argv[1],"--one-readonly-snapshot");
    if(argc!=2 || (!initPassive && !one && strcmp(argv[1],"--two-readonly-snapshots")))return 2;
    Hid::radioOnlyQueries=initPassive;
    Keyboard::State baseline,after;
    // Refuse USB-only topology before any query.
    HANDLE input=Input();if(input==INVALID_HANDLE_VALUE){puts("radio_missing queries=0 stop=1");return 3;}CloseHandle(input);
    if(one) {
        FILE* saved=fopen("verification/startup-sync-and-active/state-packets-20261012/baseline.txt","r");
        unsigned long long at=0;
        const bool loaded=saved && fscanf(saved,"%llu %d %d %d %d %d",&at,&baseline.battery,&baseline.lightEffect,&baseline.brightness,&baseline.lightSpeed,&baseline.sleepSeconds)==6;
        if(saved)fclose(saved);
        if(!loaded || GetTickCount64()<at || GetTickCount64()-at>300000){puts("saved_baseline_missing_or_old queries=0 stop=1");return 4;}
        printf("historical_baseline_age_ms=%llu baseline_queries=0\n",GetTickCount64()-at);
    } else if(!Snapshot("baseline-query",baseline)){puts("baseline_failed stop=1 setting_writes=0");return 4;}
    input=Input();if(input==INVALID_HANDLE_VALUE){puts("input_open_failed stop=1");return 5;}
    printf("passive_ready=1 tick=%llu lifetime_seconds=60 reports_limit=64 snapshot_budget=%u setting_writes=0\n",GetTickCount64(),initPassive||one?1u:2u);fflush(stdout);
    const auto deadline=GetTickCount64()+60000;unsigned reports=0;bool sampled=false,failed=false;
    while(GetTickCount64()<deadline && reports<64) {
        BYTE report[20]{};DWORD count=0;
        if(!Hid::Transfer(input,report,20,false,200,&count)) {
            const DWORD error=GetLastError();if(error==ERROR_OPERATION_ABORTED)continue;
            printf("input_failed error=%lu stop=1\n",error);failed=true;break;
        }
        ++reports;printf("unsolicited number=%u tick=%llu length=%lu bytes=",reports,GetTickCount64(),count);
        for(DWORD i=0;i<count && i<20;++i)printf("%02X ",report[i]);
        puts("");fflush(stdout);
        if(!KeyboardProtocol::WirelessFrame(report,count,report[1])){failed=true;puts("invalid_frame stop=1");break;}
        KeyboardProtocol::Power power;
        printf("unsolicited classification=observed-only power_parser_accepts=%d\n",KeyboardProtocol::WirelessPower(report,count,power));fflush(stdout);
        // The exact captured event, never a query reply or guessed numeric state.
        const BYTE observed[]={0x13,0x0A,1,0,4,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0x29};
        if(!initPassive && !sampled && std::equal(report,report+20,observed)) {
            // No outstanding input read can compete with the query/ACK channel.
            CloseHandle(input);input=INVALID_HANDLE_VALUE;Sleep(1000);sampled=true;
            if(!Snapshot("post-event-query",after)){failed=true;puts("snapshot_failed stop=1");break;}
            printf("correlation battery_changed=%d effect_changed=%d brightness_changed=%d speed_changed=%d sleep_changed=%d numeric_input_decode=0\n",
                after.battery!=baseline.battery,after.lightEffect!=baseline.lightEffect,after.brightness!=baseline.brightness,
                after.lightSpeed!=baseline.lightSpeed,after.sleepSeconds!=baseline.sleepSeconds);fflush(stdout);
            input=Input();if(input==INVALID_HANDLE_VALUE){failed=true;puts("input_reopen_failed stop=1");break;}
        }
    }
    if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    printf("state_check_complete failed=%d reports=%u snapshots=%u setting_writes=0\n",failed,reports,(one?0u:1u)+(sampled?1u:0u));fflush(stdout);
    return failed?6:0;
}
