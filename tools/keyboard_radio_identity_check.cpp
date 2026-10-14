// Bounded diagnostic: one identity or receiver-status query, one response read.
// No power/config/settings/firmware commands or standard keyboard input reads.
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include <cstdio>
#include <cstring>
#include <cassert>
int main(int argc,char** argv) {
    const bool receiver=argc==2 && std::strcmp(argv[1],"--receiver-status-only")==0;
    if(argc==2 && std::strcmp(argv[1],"--check-receiver-format")==0) {
        BYTE reply[20]{};reply[0]=0x13;reply[1]=7;reply[2]=1;reply[4]=1;reply[5]=1;reply[19]=0x1d;
        assert(KeyboardProtocol::WirelessHeader(reply,20,7,1));
        reply[19]^=1;assert(!KeyboardProtocol::WirelessHeader(reply,20,7,1));
        const BYTE notification[20]={0x13,0x0A,1,0,4,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0x29};
        assert(!KeyboardProtocol::WirelessHeader(notification,20,7,1));
        puts("receiver_format_check=passed device_io=0");return 0;
    }
    if(argc!=1 && !receiver){puts("unsupported_argument requests=0");return 2;}
    const char* label=receiver?"receiver_status":"identity";
    for(const auto& ep:Hid::Enumerate()) {
        if(ep.vid!=0x3554 || ep.pid!=0xFA09 || ep.caps.UsagePage!=0xFF02 || ep.caps.Usage!=2 || ep.caps.InputReportByteLength!=20 || ep.caps.OutputReportByteLength!=20)continue;
        SetLastError(ERROR_SUCCESS);HANDLE h=Hid::Open(ep,true);
        printf("endpoint_found=1 open_ok=%d open_error=%lu\n",h!=INVALID_HANDLE_VALUE,GetLastError());fflush(stdout);
        if(h==INVALID_HANDLE_VALUE)return 2;
        BYTE request[20]{},reply[20]{};KeyboardProtocol::WirelessRequest(receiver?7:5,request);
        DWORD sent=0;SetLastError(ERROR_SUCCESS);const auto started=GetTickCount64();
        bool wrote=Hid::Transfer(h,request,20,true,400,&sent);
        printf("%s_sent=%d bytes=%lu error=%lu elapsed=%llu request=",label,wrote,sent,GetLastError(),GetTickCount64()-started);
        for(int i=0;i<20;++i)printf("%02X%s",request[i],i==19?"\n":" ");
        fflush(stdout);
        if(!wrote){CloseHandle(h);return 3;}
        DWORD received=0;SetLastError(ERROR_SUCCESS);
        bool read=Hid::Transfer(h,reply,20,false,800,&received);
        const auto error=GetLastError();CloseHandle(h);
        bool valid=read && (receiver?KeyboardProtocol::WirelessHeader(reply,received,7,1):KeyboardProtocol::WirelessIdentity(reply,received));
        printf("%s_received=%d bytes=%lu error=%lu elapsed=%llu %s=%d reply=",label,read,received,error,GetTickCount64()-started,receiver?"format_valid":"identity_valid",valid);
        for(DWORD i=0;i<received && i<20;++i)printf("%02X%s",reply[i],i+1==received?"\n":" ");
        if(!received)puts("none");
        if(receiver)printf("receiver_status_byte=%d\n",valid?reply[5]:-1);
        if(receiver)puts("receiver_status_requests=1 config_queries=0 power_queries=0 setting_writes=0");
        else puts("identity_requests=1 config_queries=0 setting_writes=0");
        return valid?0:4;
    }
    puts("endpoint_found=0 requests=0");return 1;
}
