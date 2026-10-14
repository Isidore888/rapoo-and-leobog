// Explicit authorized physical test only. Current production checked setters
// on a serial queue, one legal rate/sleep change and restoration per field.
#include "../src/device_manager.cpp"
#include "../src/serial_tasks.h"
#include <cstdio>
#include <cstring>
#include <future>
struct Cleanup { ~Cleanup(){Device::Stop();} };
int main(int argc,char** argv) {
    bool wired=argc==2 && !strcmp(argv[1],"--verify-usb");
    if(!wired && !(argc==2 && !strcmp(argv[1],"--verify-radio")))return 2;
    if(!Device::Start(nullptr,nullptr,false))return 3;Cleanup cleanup;
    auto until=GetTickCount64()+5000;
    do {auto current=Device::GetCurrentState();if(current.isConnected && current.profileSlot>=0)break;Sleep(50);}while(GetTickCount64()<until);
    auto current=Device::GetCurrentState();
    if(!current.isConnected || current.isWired!=wired){puts("baseline_connection_mismatch no_setting_write=1");return 4;}
    BYTE originalRate=0,originalSleep=0;Device::ProfileStamp rateStamp,sleepStamp;
    if(!Device::ReadProfileSetting(Rapoo::ADDR_POLLING_HZ,originalRate,&rateStamp) ||
       !Device::ReadProfileSetting(Rapoo::ADDR_SLEEP_TIMEOUT,originalSleep,&sleepStamp) || rateStamp.slot!=sleepStamp.slot) {
        puts("baseline_read_failed no_setting_write=1");return 5;
    }
    int rate=Rapoo::CodeToPollingHz(originalRate);
    printf("baseline wired=%d rate=%d sleep_minutes=%u slot=%d\n",wired,rate,unsigned(originalSleep),rateStamp.slot);fflush(stdout);
    if(!rate || originalSleep<2 || originalSleep>120)return 6;
    SerialTasks queue;if(!queue.Start())return 7;
    for(bool sleep:{false,true}) {
        int original=sleep?originalSleep:rate;
        int changed=sleep?(original==5?10:5):(original==1000?500:1000);
        for(int target:{changed,original}) {
            auto expected=Device::GetCurrentState();std::promise<Device::SettingResult> completion;auto result=completion.get_future();
            auto begin=GetTickCount64();
            if(!queue.Submit(sleep?2:1,[&]{completion.set_value(sleep?Device::SetSleepTimeoutChecked(target,expected.connectionGeneration):
                Device::SetPollingRateChecked(target,expected.connectionGeneration));}))return 8;
            auto status=result.get();
            printf("setting=%s target=%d result=%d elapsed_ms=%llu\n",sleep?"sleep":"polling",target,int(status),GetTickCount64()-begin);fflush(stdout);
            if(status!=Device::SettingResult::Confirmed){puts("first_anomaly stop=1 no_automatic_resend=1 restoration_unconfirmed=1");return 9;}
            BYTE actualRate=0,actualSleep=0;Device::ProfileStamp afterRate,afterSleep;
            bool read=Device::ReadProfileSetting(Rapoo::ADDR_POLLING_HZ,actualRate,&afterRate) &&
                Device::ReadProfileSetting(Rapoo::ADDR_SLEEP_TIMEOUT,actualSleep,&afterSleep);
            bool equal=read && afterRate.slot==rateStamp.slot && afterSleep.slot==rateStamp.slot &&
                actualRate==(sleep?originalRate:Rapoo::PollingHzToCode(target)) && actualSleep==(sleep?target:originalSleep);
            printf("independent_rate_sleep_slot_equal=%d\n",equal);fflush(stdout);
            if(!equal){puts("first_anomaly stop=1 restoration_unconfirmed=1");return 10;}
        }
    }
    queue.Stop();puts("both_roundtrips_confirmed=1 original_rate_sleep_slot_restored=1 setting_transactions=4");return 0;
}
