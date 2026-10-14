// Bounded, zero-request vendor input observation. No standard key collections,
// feature/output requests, setting writes, automatic queries or speculative parser.
#include "../src/hid_transport.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
static std::atomic<bool> stopped{false},failed{false};
static std::atomic<unsigned> total{0};
static std::mutex logMutex;
static bool Target(const Hid::Endpoint& ep,bool radio) {
    if(radio)return ep.vid==0x3554 && ep.pid==0xFA09 && ep.caps.UsagePage==0xFF02 && ep.caps.Usage==2 &&
        ep.caps.InputReportByteLength==20 && ep.caps.OutputReportByteLength==20;
    return ep.vid==0x258A && ep.pid==0x010C && ep.caps.UsagePage==0xFF00 && ep.caps.Usage==1 &&
        (ep.caps.InputReportByteLength==4 || ep.caps.InputReportByteLength==8) && !ep.caps.OutputReportByteLength;
}
static void Observe(Hid::Endpoint ep,unsigned id,ULONGLONG until) {
    HANDLE handle=CreateFileW(ep.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(handle==INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(logMutex);printf("open_failed endpoint=%u error=%lu stop=1\n",id,GetLastError());
        failed=true;stopped=true;fflush(stdout);return;
    }
    unsigned reports=0;std::vector<BYTE> bytes(ep.caps.InputReportByteLength);
    {std::lock_guard<std::mutex> lock(logMutex);printf("listening endpoint=%u\n",id);fflush(stdout);}
    while(!stopped && GetTickCount64()<until) {
        DWORD count=0;
        if(!Hid::Transfer(handle,bytes.data(),DWORD(bytes.size()),false,200,&count)) {
            const DWORD error=GetLastError();
            if(error==ERROR_OPERATION_ABORTED)continue; // drained idle timeout
            std::lock_guard<std::mutex> lock(logMutex);printf("read_failed endpoint=%u error=%lu stop=1\n",id,error);
            failed=true;stopped=true;fflush(stdout);break;
        }
        std::lock_guard<std::mutex> lock(logMutex);
        ++reports;const unsigned number=++total;
        printf("report endpoint=%u number=%u tick=%llu length=%lu bytes=",id,number,GetTickCount64(),count);
        for(DWORD i=0;i<count && i<bytes.size();++i)printf("%02X ",bytes[i]);
        puts("");fflush(stdout);
        if(count!=bytes.size() || number>=64) {
            puts("invalid_length_or_report_budget stop=1");failed=true;stopped=true;
        }
    }
    CloseHandle(handle);
    std::lock_guard<std::mutex> lock(logMutex);printf("finished endpoint=%u reports=%u\n",id,reports);fflush(stdout);
}
int main(int argc,char** argv) {
    if(argc!=2 || (strcmp(argv[1],"--usb") && strcmp(argv[1],"--radio")))return 2;
    const bool radio=!strcmp(argv[1],"--radio");std::vector<Hid::Endpoint> endpoints;
    for(const auto& ep:Hid::Enumerate())if(Target(ep,radio))endpoints.push_back(ep);
    if(endpoints.empty()){puts("target_missing requests=0 stop=1");return 3;}
    // Avoid duplicate or unexpected topology rather than listening arbitrarily.
    if(endpoints.size()>(radio?1u:2u)){puts("unexpected_topology requests=0 stop=1");return 4;}
    for(unsigned i=0;i<endpoints.size();++i) {
        const auto& ep=endpoints[i];printf("endpoint=%u vid=%04X pid=%04X usage_page=%04X usage=%04X input=%u output=%u feature=%u\n",
            i,ep.vid,ep.pid,ep.caps.UsagePage,ep.caps.Usage,ep.caps.InputReportByteLength,ep.caps.OutputReportByteLength,ep.caps.FeatureReportByteLength);
    }
    printf("observation_start tick=%llu duration_seconds=45 report_budget=64 requests=0 setting_writes=0 mode=%s\n",GetTickCount64(),radio?"radio":"usb");fflush(stdout);
    const auto until=GetTickCount64()+45000;std::vector<std::thread> readers;
    for(unsigned i=0;i<endpoints.size();++i)readers.emplace_back(Observe,endpoints[i],i,until);
    for(auto& reader:readers)reader.join();
    printf("observation_complete failed=%d reports=%u requests=0 setting_writes=0\n",failed.load(),total.load());fflush(stdout);
    return failed?5:0;
}
