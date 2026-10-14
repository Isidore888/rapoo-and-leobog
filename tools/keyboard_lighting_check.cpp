// Opt-in check: close the tray first. Default is read-only. --roundtrip briefly
// changes each supported light setting by one step, then restores the read value.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef KEYBOARD_READ_DIAGNOSTIC
#include "../src/hid_transport.h"
namespace Hid {
static bool TracedTransfer(HANDLE handle,BYTE* data,DWORD size,bool write,DWORD timeout,DWORD* received=nullptr) {
    DWORD count=0;const auto begin=GetTickCount64();SetLastError(ERROR_SUCCESS);
    bool ok=Transfer(handle,data,size,write,timeout,&count);const auto error=GetLastError();
    printf("transport direction=%s ok=%d count=%lu error=%lu elapsed=%llu data=",write?"query":"reply",ok,count,error,GetTickCount64()-begin);
    for(DWORD i=0;i<(write?size:count) && i<size;++i)printf("%02X%s",data[i],i+1==(write?size:count)?"\n":" ");
    if(!write && !count)puts("none");fflush(stdout);
    if(received)*received=count;return ok;
}
}
#define Transfer TracedTransfer
#endif
#include "../src/keyboard_manager.cpp"
#ifdef KEYBOARD_READ_DIAGNOSTIC
#undef Transfer
#endif
int main(int argc,char** argv) {
    auto current=Keyboard::Query();
    printf("connected=%d wired=%d battery=%d light_valid=%d effect=%d brightness=%d speed=%d sleep_valid=%d sleep_seconds=%d config_result=%d\n",
        current.isConnected,current.isWired,current.battery,current.lightingValid,current.lightEffect,current.brightness,current.lightSpeed,current.sleepValid,current.sleepSeconds,int(current.configResult));
    if(!current.lightingValid)return 2;
    if(argc>1 && strncmp(argv[1],"--sleep=",8)==0) {
        char* end=nullptr;long seconds=strtol(argv[1]+8,&end,10);
        if(end==argv[1]+8 || *end || seconds<30 || seconds>7200 || seconds%30)return 4;
        auto result=Keyboard::SetSleepTimeout(int(seconds));auto after=Keyboard::Query();
        printf("sleep requested=%ld result=%d final=%d effect=%d brightness=%d speed=%d\n",seconds,int(result),after.sleepSeconds,after.lightEffect,after.brightness,after.lightSpeed);
        return result==Keyboard::LightingResult::Confirmed && after.sleepValid && after.sleepSeconds==seconds && after.lightEffect==current.lightEffect && after.brightness==current.brightness && after.lightSpeed==current.lightSpeed?0:3;
    }
    if(argc>1 && strcmp(argv[1],"--sleep-roundtrip")==0) {
        if(!current.sleepValid)return 2;
        int changed=current.sleepSeconds==300?600:300;
        auto result=Keyboard::SetSleepTimeout(changed);
        printf("sleep change %d->%d seconds result=%d\n",current.sleepSeconds,changed,int(result));
        auto restore=Keyboard::SetSleepTimeout(current.sleepSeconds);
        printf("sleep restore=%d seconds result=%d\n",current.sleepSeconds,int(restore));
        auto after=Keyboard::Query();
        printf("final wired=%d battery=%d sleep=%d effect=%d brightness=%d speed=%d\n",after.isWired,after.battery,after.sleepSeconds,after.lightEffect,after.brightness,after.lightSpeed);
        return result==Keyboard::LightingResult::Confirmed && restore==Keyboard::LightingResult::Confirmed && after.sleepValid && after.sleepSeconds==current.sleepSeconds && after.lightEffect==current.lightEffect && after.brightness==current.brightness && after.lightSpeed==current.lightSpeed?0:3;
    }
    if(argc>1 && strcmp(argv[1],"--dump")==0) {
        for(const auto& ep:Hid::Enumerate()) {
            bool wired=ep.vid==0x258A && ep.pid==0x010C && ep.caps.UsagePage==0xFF00 && ep.caps.Usage==1 && ep.caps.FeatureReportByteLength==520;
            bool radio=ep.vid==0x3554 && ep.pid==0xFA09 && ep.caps.UsagePage==0xFF02 && ep.caps.Usage==2 && ep.caps.InputReportByteLength==20 && ep.caps.OutputReportByteLength==20;
            if(!(current.isWired?wired:radio))continue;
            HANDLE h=Hid::Open(ep,!wired);if(h==INVALID_HANDLE_VALUE)continue;
            BYTE reply[520]{},page[128]{};
            bool identity=wired?Keyboard::WiredExchange(h,0x82,1,6,reply)&&KeyboardProtocol::WiredIdentity(reply,520):Keyboard::WirelessExchange(h,5,reply,true);
            bool ok=identity && (wired?Keyboard::ReadWiredLighting(h,page):Keyboard::ReadWirelessLighting(h,page));CloseHandle(h);
            if(!ok)return 3;
            printf("sleep candidate raw=%d\n",page[24]);
            for(int i=0;i<128;++i)printf("%02X%s",page[i],i%16==15?"\n":" ");
            return 0;
        }
        return 4;
    }
    if(argc>1 && strcmp(argv[1],"--same")==0) {
        auto result=Keyboard::SetLighting(false,current.brightness);
        printf("same brightness=%d result=%d\n",current.brightness,int(result));
        return result==Keyboard::LightingResult::Confirmed?0:3;
    }
    if(argc<2 || strcmp(argv[1],"--roundtrip")!=0)return 0;
    for(bool speed:{false,true}) {
        int original=speed?current.lightSpeed:current.brightness;
        int changed=original>0?original-1:1;
        auto result=Keyboard::SetLighting(speed,changed);
        printf("%s change %d->%d result=%d\n",speed?"speed":"brightness",original,changed,int(result));
        auto restore=Keyboard::SetLighting(speed,original);
        printf("%s restore=%d result=%d\n",speed?"speed":"brightness",original,int(restore));
        if(result!=Keyboard::LightingResult::Confirmed || restore!=Keyboard::LightingResult::Confirmed)return 3;
    }
    auto after=Keyboard::Query();
    printf("final effect=%d brightness=%d speed=%d valid=%d\n",after.lightEffect,after.brightness,after.lightSpeed,after.lightingValid);
    return after.lightingValid && after.lightEffect==current.lightEffect && after.brightness==current.brightness && after.lightSpeed==current.lightSpeed?0:4;
}
