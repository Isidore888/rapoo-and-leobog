#include "rapoo_protocol.h"
#include <cstring>
#include <algorithm>

namespace Rapoo {

DWORD BuildUnlockPacket(BYTE* outBuf, DWORD bufSize, bool wired) {
    if (!outBuf || bufSize < 33) return 0;
    std::memset(outBuf, 0, bufSize);
    outBuf[0] = REPORT_ID_CONTROL; // 0x06
    outBuf[1] = wired ? 0xFF : 0xA5;
    outBuf[2] = 0xA3;
    outBuf[3] = 0x00;
    outBuf[4] = 0x00;
    outBuf[5] = 0x00;
    outBuf[6] = 0x00;
    outBuf[7] = 0x00;
    outBuf[8] = 0x00;
    return 33;
}

DWORD BuildWriteCommand(BYTE bank, BYTE addr, const BYTE* pData, BYTE dataLen, BYTE* outBuf, DWORD bufSize, bool wired) {
    if (!outBuf || bufSize < 33) return 0;
    std::memset(outBuf, 0, bufSize);
    outBuf[0] = REPORT_ID_CONTROL; // 0x06
    outBuf[1] = wired ? 0xFF : 0xA5;
    outBuf[2] = 0xA5;
    outBuf[3] = (BYTE)(dataLen & 0xFF);
    outBuf[4] = addr;
    outBuf[5] = bank;
    outBuf[6] = 0x00;
    outBuf[7] = 0x00;
    if (pData && dataLen > 0) {
        BYTE copyLen = (dataLen > 25) ? 25 : dataLen;
        std::memcpy(&outBuf[8], pData, copyLen);
    }
    return 33;
}

DWORD BuildReadCommand(BYTE bank, BYTE addr, BYTE readLen, BYTE* outBuf, DWORD bufSize, bool wired) {
    if (!outBuf || bufSize < 33) return 0;
    std::memset(outBuf, 0, bufSize);
    outBuf[0] = REPORT_ID_CONTROL; // 0x06
    outBuf[1] = wired ? 0xFF : 0xA5;
    outBuf[2] = 0xA4;
    outBuf[3] = readLen;
    outBuf[4] = addr;
    outBuf[5] = bank;
    outBuf[6] = 0x00;
    outBuf[7] = 0x00;
    outBuf[8] = 0x00;
    return 33;
}

bool ParseStatusReport(const BYTE* buf, DWORD bytesRead, DeviceStatus& outStatus, int cachedBattery) {
    if (!buf || bytesRead != 19) return false;
    if (buf[0] != REPORT_ID_STATUS) return false;
    // Device marker: 0x20 (2.4G dongle), 0x10 (wired USB direct).
    // High nibble contains device mode; lower nibble carries dynamic status bits.
    BYTE modeNibble = buf[1] & 0xF0;
    // Master V2 reports 0x40/0x42 on the confirmed 146E/466E endpoints.
    if (modeNibble != 0x20 && modeNibble != 0x10 && modeNibble != 0x40) return false;
    outStatus.isWired = (modeNibble == 0x10);
    // A Hub's report parser omits the report ID: rpt[10]/cfgSel[11]
    // therefore become bytes 11/12 in the Windows report. Confirmed V2 only.
    outStatus.pollingHz = modeNibble == 0x40 ? CodeToPollingHz(buf[11]) : 0;
    outStatus.profileSlot = modeNibble == 0x40 && buf[12] <= 7 ? buf[12] : -1;

    outStatus.dpiLevel = (int)buf[2] + 1;
    outStatus.dpiX = (int)buf[3] | ((int)buf[4] << 8);
    outStatus.dpiY = (int)buf[5] | ((int)buf[6] << 8);

    BYTE statusByte = buf[7];
    BYTE rawBat = buf[8];
    BYTE extraByte = (bytesRead > 9) ? buf[9] : 0;

    // Charging flag detection based on Rapoo status flags
    bool charging = ((statusByte & 0x02) != 0 || statusByte == 0x02 || statusByte == 0x03 ||
                     extraByte == 0x01 || extraByte == 0x02);
    outStatus.isCharging = modeNibble == 0x40 ? (buf[1] & 0x02) != 0 : charging;

    // A valid zero is empty; never substitute unknown data with a full battery.
    if (rawBat > 100) return false;
    outStatus.battery = rawBat;
    (void)cachedBattery;

    return true;
}

bool ParseFeatureRead(const BYTE* buf, DWORD bytesRead, BYTE expectedLen, BYTE* outData) {
    if (!buf || bytesRead < 5) return false;

    // Case 1: Windows HID driver stripped Report ID -> buf[0] is ACK (0x01)
    if (buf[0] == 0x01 && bytesRead >= (DWORD)(4 + expectedLen)) {
        if (outData) {
            std::memcpy(outData, &buf[4], expectedLen);
        }
        return true;
    }

    // Case 2: buf[0] is Report ID (0x08) and buf[1] is ACK (0x01)
    if (buf[0] == REPORT_ID_FEATURE && buf[1] == 0x01 && bytesRead >= (DWORD)(5 + expectedLen)) {
        if (outData) {
            std::memcpy(outData, &buf[5], expectedLen);
        }
        return true;
    }

    return false;
}

int CodeToPollingHz(BYTE code) {
    switch (code) {
        case 0x08: return 125;
        case 0x04: return 250;
        case 0x02: return 500;
        case 0x01: return 1000;
        case 0x84: return 2000;
        case 0x82: return 4000;
        case 0x81: return 8000;
        default:   return 0;
    }
}

BYTE PollingHzToCode(int hz) {
    switch (hz) {
        case 125:  return 0x08;
        case 250:  return 0x04;
        case 500:  return 0x02;
        case 1000: return 0x01;
        case 2000: return 0x84;
        case 4000: return 0x82;
        case 8000: return 0x81;
        default:   return 0x01;
    }
}

} // namespace Rapoo
