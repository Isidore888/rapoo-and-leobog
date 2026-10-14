#pragma once
#include "battery_state.h"
#include "keyboard_protocol.h"
namespace Keyboard {
constexpr UINT SettingsChangedMessage=WM_APP+6;
constexpr DWORD BrightnessChanged=1,SpeedChanged=2;
enum class LightingResult { Confirmed, InvalidValue, Unavailable, ReadFailed, WriteFailed, ReadbackFailed,
    HeaderMismatch, UnsupportedConfig, ReadbackMismatch, WriteAckFailed, Superseded };
struct State {
    ULONGLONG connectionGeneration=0;
    bool isConnected = false, isWired = false, isCharging = false, chargingKnown = false, batteryValid = false;
    int battery = -1;
    ULONGLONG lastBatteryUpdate = 0;
    bool lightingValid=false,lightingWritable=false;
    bool lightingStale=false,sleepStale=false;
    LightingResult configResult=LightingResult::Unavailable;
    int brightness=-1,lightSpeed=-1,lightEffect=-1;
    ULONGLONG lastLightingUpdate=0;
    ULONGLONG lightingRevision=0;
    bool sleepValid=false;
    int sleepSeconds=-1;
    ULONGLONG lastSleepUpdate=0;
    wchar_t modelName[64] = L"LEOBOG HI98C Pro";
};
// A reconnect, expired baseline or effect switch is a new snapshot, not a hotkey.
inline DWORD LightingChanges(const State& before,const State& after,ULONGLONG now=GetTickCount64()) {
    if(!before.isConnected || !after.isConnected || before.lightingStale || after.lightingStale || before.isWired!=after.isWired || before.lightEffect!=after.lightEffect ||
        !Battery::Fresh(before.lightingValid,before.lastLightingUpdate,now) || !Battery::Fresh(after.lightingValid,after.lastLightingUpdate,now))return 0;
    return (before.brightness!=after.brightness?BrightnessChanged:0) | (before.lightSpeed!=after.lightSpeed?SpeedChanged:0);
}
bool Start(HWND notifyWindow, UINT updateMessage);
void Stop();
void BeginShutdown();
void ForceRefresh();
void NotifyDeviceChange();
State GetCurrentState();
// Single-shot read-only query, also used by the hardware verification tool.
State Query();
LightingResult SetLighting(bool speed,int level);
LightingResult SetSleepTimeout(int seconds);
LightingResult SetLightingChecked(bool speed,int level,const State& expected);
LightingResult SetSleepTimeoutChecked(int seconds,const State& expected);
void FormatSettingPopup(KeyboardProtocol::Setting setting,int value,LightingResult result,wchar_t* title,size_t titleSize,wchar_t* detail,size_t detailSize);
const wchar_t* LightingResultText(LightingResult result);
}
