// Explicit hardware opt-in only. Production guarded setter runs on its queue.
// One change then restoration per field, full-page comparison at each step.
// Stop on first anomaly; never resend an unconfirmed setting automatically.
#include "../src/keyboard_manager.cpp"
#include "../src/serial_tasks.h"
#include <cstdio>
#include <cstring>
#include <future>

static bool ReadPage(bool wired,BYTE* page) {
    for(const auto& ep:Hid::Enumerate()) {
        const auto& c=ep.caps;
        bool match=wired?ep.vid==0x258A && ep.pid==0x010C && c.UsagePage==0xFF00 && c.Usage==1 && c.FeatureReportByteLength==520:
            ep.vid==0x3554 && ep.pid==0xFA09 && c.UsagePage==0xFF02 && c.Usage==2 && c.InputReportByteLength==20 && c.OutputReportByteLength==20;
        if(!match)continue;
        HANDLE h=Hid::Open(ep,!wired);if(h==INVALID_HANDLE_VALUE)return false;
        BYTE reply[520]{};
        bool identity=wired?Keyboard::WiredExchange(h,0x82,1,6,reply) && KeyboardProtocol::WiredIdentity(reply,520):Keyboard::WirelessExchange(h,5,reply,true);
        bool ok=identity && (wired?Keyboard::ReadWiredLighting(h,page):Keyboard::ReadWirelessLighting(h,page));
        CloseHandle(h);return ok;
    }
    return false;
}
int main(int argc,char** argv) {
    bool wired=argc==2 && !strcmp(argv[1],"--verify-usb");
    if(!wired && !(argc==2 && !strcmp(argv[1],"--verify-radio")))return 2;
    auto original=Keyboard::Query();
    printf("baseline connected=%d wired=%d brightness=%d speed=%d sleep_seconds=%d effect=%d\n",
           original.isConnected,original.isWired,original.brightness,original.lightSpeed,original.sleepSeconds,original.lightEffect);fflush(stdout);
    if(!original.isConnected || original.isWired!=wired || !original.lightingValid || !original.sleepValid ||
       original.brightness!=4 || original.lightSpeed!=4 || original.sleepSeconds!=360) {
        puts("baseline_unavailable_or_outside_authorized_values no_setting_write=1");return 3;
    }
    BYTE baseline[128]{};KeyboardProtocol::Lighting light;int seconds=0;
    if(!ReadPage(wired,baseline) || !KeyboardProtocol::ParseLighting(baseline,128,light) ||
       !KeyboardProtocol::ParseSleep(baseline,128,seconds) || light.effect!=original.lightEffect || light.brightness!=4 || light.speed!=4 || seconds!=360) {
        puts("complete_baseline_mismatch no_setting_write=1");return 4;
    }
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=original;}
    SerialTasks queue;if(!queue.Start()){puts("queue_start_failed no_setting_write=1");return 5;}
    for(auto setting:{KeyboardProtocol::Setting::Brightness,KeyboardProtocol::Setting::Speed,KeyboardProtocol::Setting::Sleep}) {
        const int before=setting==KeyboardProtocol::Setting::Sleep?360:4;
        const int changed=setting==KeyboardProtocol::Setting::Sleep?300:3;
        const char* name=setting==KeyboardProtocol::Setting::Brightness?"brightness":setting==KeyboardProtocol::Setting::Speed?"speed":"sleep";
        for(int target:{changed,before}) {
            auto expected=Keyboard::GetCurrentState();std::promise<Keyboard::LightingResult> completion;
            auto result=completion.get_future();const auto begin=GetTickCount64();
            if(!queue.Submit(unsigned(setting)+1,[&]{completion.set_value(setting==KeyboardProtocol::Setting::Sleep?
                Keyboard::SetSleepTimeoutChecked(target,expected):Keyboard::SetLightingChecked(setting==KeyboardProtocol::Setting::Speed,target,expected));})) {
                puts("queue_submit_failed stop=1");return 6;
            }
            auto status=result.get();
            printf("setting=%s target=%d result=%d elapsed_ms=%llu\n",name,target,int(status),GetTickCount64()-begin);fflush(stdout);
            if(status!=Keyboard::LightingResult::Confirmed) {
                puts("first_anomaly stop=1 no_automatic_resend=1 restoration_unconfirmed=1");return 7;
            }
            BYTE actual[128]{},wanted[128];std::copy(baseline,baseline+128,wanted);
            bool patched=setting==KeyboardProtocol::Setting::Sleep?KeyboardProtocol::PatchSleep(wanted,128,target):
                KeyboardProtocol::PatchLighting(wanted,128,setting==KeyboardProtocol::Setting::Speed,target);
            bool read=ReadPage(wired,actual),equal=read && patched && std::equal(actual,actual+128,wanted);
            printf("setting=%s target=%d independent_full_page_equal=%d\n",name,target,equal);fflush(stdout);
            if(!equal){puts("first_anomaly stop=1 restoration_unconfirmed=1");return 8;}
        }
    }
    queue.Stop();puts("all_three_roundtrips_confirmed=1 full_original_page_restored=1 setting_transactions=6 no_firmware_keymap_reset=1");return 0;
}
