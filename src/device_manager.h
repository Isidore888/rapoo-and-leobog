#pragma once
#include <windows.h>
#include "rapoo_protocol.h"
#include "battery_state.h"

namespace Device {

struct State {
    bool isConnected = false;
    bool isWired = false;
    bool isCharging = false;
    int battery = -1;
    bool batteryValid = false;
    ULONGLONG lastBatteryUpdate = 0;
    int dpiLevel = 1;
    int dpiX = 800;
    int dpiY = 800;
    int pollingHz = 0; // zero means no confirmed hardware reading
    int sleepMinutes = 0;
    int profileSlot = -1;
    ULONGLONG connectionGeneration = 0; // connection/mode/profile context
    WCHAR modelName[64] = L"雷柏 VT3S MAX 大师版 V2";
};

typedef void (*StateCallback)(const State& state, DWORD changeMask);

constexpr DWORD CHANGE_CONNECTED = 0x01;
constexpr DWORD CHANGE_BATTERY   = 0x02;
constexpr DWORD CHANGE_DPI       = 0x04;
constexpr DWORD CHANGE_POLLING   = 0x08;
constexpr DWORD CHANGE_SLEEP     = 0x10;

bool Start(HWND hNotifyWnd, StateCallback callback, bool allowSettingsQueries = true);
void Stop();
void BeginShutdown();

void NotifyDeviceChange();

bool SetPollingRate(int hz);
bool SetSleepTimeout(int minutes);
enum class SettingResult {
    Confirmed, InvalidValue, Disconnected, InterfaceUnavailable, RetryLater,
    RecoveryFailed, ProfileReadFailed, ProfileChanged, WriteFailed, WriteAckFailed,
    ReadbackFailed, ReadbackMismatch, Superseded, CurrentReadFailed
};
SettingResult SetPollingRateChecked(int hz,ULONGLONG expectedGeneration=~ULONGLONG(0));
SettingResult SetSleepTimeoutChecked(int minutes,ULONGLONG expectedGeneration=~ULONGLONG(0));
const WCHAR* SettingResultText(SettingResult result);
bool RefreshPollingRate();
void ForceRefresh();
// Host's mouse queue also owns control reads; input reception never waits for it.
void SetControlScheduler(void (*schedule)(bool));
void RunControlRefresh(bool request);

State GetCurrentState();

} // namespace Device
