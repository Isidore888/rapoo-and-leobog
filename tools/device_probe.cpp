#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <cstdio>
int main() {
    for (const auto& ep : Hid::Enumerate()) {
        const auto& c = ep.caps;
        printf("%04X:%04X usage=%04X/%04X in=%u out=%u feature=%u\n", ep.vid, ep.pid, c.UsagePage, c.Usage, c.InputReportByteLength, c.OutputReportByteLength, c.FeatureReportByteLength);
        bool mouse = ep.vid == 0x24AE && c.UsagePage == 0xFF00 && c.Usage == 2;
        bool wired = ep.vid == 0x258A && ep.pid == 0x010C && c.UsagePage == 0xFF00 && c.Usage == 1 && c.FeatureReportByteLength == 520;
        bool wireless = ep.vid == 0x3554 && ep.pid == 0xFA09 && c.UsagePage == 0xFF02 && c.Usage == 2 && c.InputReportByteLength == 20 && c.OutputReportByteLength == 20;
        if (!mouse && !wired && !wireless) continue;
        HANDLE h = Hid::Open(ep, !wired); if (h == INVALID_HANDLE_VALUE) { printf("  open failed %lu\n", GetLastError()); continue; }
        BYTE p[520]{};
        if (wired) {
            for (int cmd : {0x82, 0x87}) {
                KeyboardProtocol::WiredRequest(cmd, cmd == 0x82 ? 1 : 2, cmd == 0x82 ? 6 : 2, p);
                if (HidD_SetFeature(h, p, 520)) {
                    Sleep(25); std::fill(p, p+520, 0); p[0] = 6;
                    if (HidD_GetFeature(h, p, 520)) { printf("  wired reply:"); for (int i=0;i<16;++i) printf(" %02X",p[i]); puts(""); }
                }
            }
        } else if (wireless) {
            for (int cmd : {5, 0x0B}) {
                KeyboardProtocol::WirelessRequest(cmd, p);
                if (Hid::Transfer(h,p,20,true,800)) {
                    DWORD n=0;
                    if (Hid::Transfer(h,p,20,false,800,&n)) {printf("  wireless reply:"); for(DWORD i=0;i<n;++i)printf(" %02X",p[i]);puts("");}
                }
            }
        } else {
            DWORD n=0;
            if (Hid::Transfer(h,p,c.InputReportByteLength,false,2500,&n)) {printf("  passive mouse:"); for(DWORD i=0;i<n;++i)printf(" %02X",p[i]);puts("");}
        }
        CloseHandle(h);
    }
}
