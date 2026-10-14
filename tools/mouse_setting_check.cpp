// Explicit opt-in hardware check. Close the tray first; never run in the default
// test suite. Read the current sleep setting, write that same value once and
// confirm it. No keyboard access, write retries, profile changes or firmware commands.
#include "../src/device_manager.cpp"
#include <cstdio>
int main() {
    if(!Device::Start(nullptr,nullptr,false)){puts("FAIL: reader startup");return 1;}
    for(int wait=0;wait<80 && !Device::GetCurrentState().isConnected;++wait)Sleep(100);
    BYTE original=0;
    bool read=Device::ReadProfileSetting(Rapoo::ADDR_SLEEP_TIMEOUT,original);
    printf("initial_sleep_read=%d minutes=%u\n",read,unsigned(original));
    if(!read || original<2 || original>120){Device::Stop();puts("FAIL: no write attempted");return 2;}
    auto result=Device::SetSleepTimeoutChecked(original);
    auto state=Device::GetCurrentState();
    printf("same_value_write_result=%d confirmed_minutes=%d connected=%d wired=%d polling=%d slot=%d\n",
        int(result),state.sleepMinutes,state.isConnected,state.isWired,state.pollingHz,state.profileSlot);
    Device::Stop();
    return result==Device::SettingResult::Confirmed && state.sleepMinutes==original?0:3;
}
