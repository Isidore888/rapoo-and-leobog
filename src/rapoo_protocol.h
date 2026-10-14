#pragma once
#include <windows.h>
#include <cstdint>

namespace Rapoo {
// A Hub configuration slot bases are 0x600 + slot*0x400.
// Polling/sleep offsets 0x280/0x2C2 therefore share bank 8 + slot*4.
inline bool ProfileBank(BYTE slot, BYTE& bank) {
    if(slot>7)return false;
    bank=BYTE(8+slot*4);return true;
}

// Standard Rapoo HID Report IDs (aligned with ClickSync)
constexpr BYTE REPORT_ID_CONTROL = 0x06; // Output report for sending commands
constexpr BYTE REPORT_ID_STATUS  = 0x07; // Input report for passive status broadcasts
constexpr BYTE REPORT_ID_FEATURE = 0x08; // Feature report for reading register responses

// Settings in the active profile bank (see ProfileBank).
constexpr BYTE ADDR_POLLING_HZ = 0x80;
constexpr BYTE ADDR_SLEEP_TIMEOUT = 0xC2; // 2..120 minutes

struct DeviceStatus {
    int dpiLevel = 1;
    int dpiX = 800;
    int dpiY = 800;
    int battery = -1;
    bool isCharging = false;
    bool isWired = false; // device marker 0x10=wired, 0x20=2.4G dongle
    int pollingHz = 0;
    int profileSlot = -1;
};

// Packet Builders
// Legacy name: A3 is a firmware query, not an unlock handshake.
DWORD BuildUnlockPacket(BYTE* outBuf, DWORD bufSize, bool wired = false);
DWORD BuildWriteCommand(BYTE bank, BYTE addr, const BYTE* pData, BYTE dataLen, BYTE* outBuf, DWORD bufSize, bool wired = false);
DWORD BuildReadCommand(BYTE bank, BYTE addr, BYTE readLen, BYTE* outBuf, DWORD bufSize, bool wired = false);

// Parsers
bool ParseStatusReport(const BYTE* buf, DWORD bytesRead, DeviceStatus& outStatus, int cachedBattery);
bool ParseFeatureRead(const BYTE* buf, DWORD bytesRead, BYTE expectedLen, BYTE* outData);

// Polling Rate Converters
int CodeToPollingHz(BYTE code);
BYTE PollingHzToCode(int hz);

// Sleep Timeout Clamping (2 to 120 minutes)
inline int ClampSleepMinutes(int minutes) {
    if (minutes < 2) return 2;
    if (minutes > 120) return 120;
    return minutes;
}

} // namespace Rapoo
