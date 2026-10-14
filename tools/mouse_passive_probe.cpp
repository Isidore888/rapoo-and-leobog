#include "../src/hid_transport.h"
#include "../src/rapoo_protocol.h"
#include <cstdio>
// Status input only: no control or feature handles, no output reports.
int main() {
    for(USHORT pid:{USHORT(0x466E),USHORT(0x146E)}) {
        for(const auto& ep:Hid::Enumerate()) {
            if(ep.vid!=0x24AE || ep.pid!=pid || ep.caps.UsagePage!=0xFF00 || ep.caps.Usage!=2 || ep.caps.InputReportByteLength!=19)continue;
            HANDLE h=CreateFileW(ep.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(h==INVALID_HANDLE_VALUE)continue;
            BYTE report[19]{};DWORD size=0;
            bool ok=Hid::Transfer(h,report,sizeof(report),false,15000,&size);CloseHandle(h);
            if(!ok){puts("No passive status within 15 seconds; no commands sent");return 1;}
            printf("PID=%04X bytes=%lu status:",pid,size);for(DWORD i=0;i<size;++i)printf(" %02X",report[i]);puts("");return 0;
        }
    }
    puts("No target status endpoint; no commands sent");return 1;
}
