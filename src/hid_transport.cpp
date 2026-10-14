#include "hid_transport.h"
#include "diagnostics.h"
#include <setupapi.h>
#ifndef IOCTL_HID_GET_FEATURE
#include <hidclass.h>
#endif
namespace Hid {
std::vector<Endpoint> Enumerate() {
    const auto started=GetTickCount64();Diagnostics::Log("enumerate_begin");
    std::vector<Endpoint> result;
    GUID guid; HidD_GetHidGuid(&guid);
    HDEVINFO list = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (list == INVALID_HANDLE_VALUE) {Diagnostics::Log("enumerate_end",0,0,0,0,GetLastError(),0,GetTickCount64()-started);return result;}
    SP_DEVICE_INTERFACE_DATA info{}; info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(list, nullptr, &guid, i, &info); ++i) {
        DWORD size = 0; SetupDiGetDeviceInterfaceDetailW(list, &info, nullptr, 0, &size, nullptr);
        if (!size) continue;
        std::vector<BYTE> buffer(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(list, &info, detail, size, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{}; attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetAttributes(h, &attr) && (attr.VendorID == 0x24AE || attr.VendorID == 0x258A || attr.VendorID == 0x3554) && HidD_GetPreparsedData(h, &pp)) {
            Endpoint ep; ep.path = detail->DevicePath; ep.vid = attr.VendorID; ep.pid = attr.ProductID;
            if (HidP_GetCaps(pp, &ep.caps) == HIDP_STATUS_SUCCESS) result.push_back(ep);
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(list);
    Diagnostics::Log("enumerate_end",0,0,0,1,0,result.size(),GetTickCount64()-started);
    return result;
}
HANDLE Open(const Endpoint& ep, bool overlapped) {
    Diagnostics::Log("open_begin",0,0,0,overlapped,0,(unsigned(ep.vid)<<16)|ep.pid);
    HANDLE h=CreateFileW(ep.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr);
    Diagnostics::Log("open_end",0,0,0,h!=INVALID_HANDLE_VALUE,h==INVALID_HANDLE_VALUE?GetLastError():0,(unsigned(ep.vid)<<16)|ep.pid);
    return h;
}
bool Transfer(HANDLE h, BYTE* data, DWORD size, bool write, DWORD timeout, DWORD* received) {
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, data, size, &done, &ov) : ReadFile(h, data, size, &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout) == WAIT_OBJECT_0) ok = GetOverlappedResult(h, &ov, &done, FALSE);
        else {
            CancelIoEx(h, &ov);
            // Drain cancellation before the stack OVERLAPPED and buffer go out of scope.
            GetOverlappedResult(h, &ov, &done, TRUE);
            ok = FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    if (received) *received = done;
    return ok && (write ? done == size : done > 0);
}
bool Feature(HANDLE h, BYTE* data, DWORD size, bool write, DWORD timeout, HANDLE stop, DWORD* received) {
    const auto id=++Diagnostics::ids;
    const BYTE cmd=Diagnostics::query?Diagnostics::command:(write && size>1?data[1]:0);
    const BYTE param=Diagnostics::query?Diagnostics::parameter:0;
    Diagnostics::activeUsbIo=id;
    Diagnostics::Log(write?"feature_set_begin":"feature_get_begin",id,cmd,param,0,0,timeout);
    const auto started=GetTickCount64();
    struct End {
        unsigned long long id;BYTE cmd,param;ULONGLONG started;bool write;
        bool accepted=false;DWORD error=0,done=0;
        ~End(){Diagnostics::Log(write?"feature_set_end":"feature_get_end",id,cmd,param,accepted,error,done,GetTickCount64()-started);Diagnostics::activeUsbIo=0;}
    } trace{id,cmd,param,started,write};
    if(received)*received=0;
    if(!timeout || (stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0)){trace.error=timeout?ERROR_OPERATION_ABORTED:ERROR_TIMEOUT;return false;}
    const auto deadline=GetTickCount64()+timeout;
    OVERLAPPED ov{};ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!ov.hEvent){trace.error=GetLastError();return false;}
    DWORD done=0;
    BOOL ok=DeviceIoControl(h,write?IOCTL_HID_SET_FEATURE:IOCTL_HID_GET_FEATURE,
        data,size,write?nullptr:data,write?0:size,&done,&ov);
    trace.error=ok?0:GetLastError();
    if(!ok && trace.error==ERROR_IO_PENDING) {
        HANDLE events[]={ov.hEvent,stop};
        const auto now=GetTickCount64();
        DWORD wait=WaitForMultipleObjects(stop?2:1,events,FALSE,DWORD(deadline>now?deadline-now:0));
        if(wait==WAIT_OBJECT_0){ok=GetOverlappedResult(h,&ov,&done,FALSE);trace.error=ok?0:GetLastError();}
        else {
            DWORD waitError=wait==WAIT_FAILED?GetLastError():0;
            BOOL canceled=CancelIoEx(h,&ov);DWORD cancelError=canceled?0:GetLastError();
            // The driver must finish cancellation before these buffers can be released.
            BOOL drained=GetOverlappedResult(h,&ov,&done,TRUE);DWORD drainError=drained?0:GetLastError();
            ok=FALSE;
            trace.error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:wait==WAIT_OBJECT_0+1?ERROR_OPERATION_ABORTED:waitError;
            Diagnostics::Log("feature_cancel",id,cmd,param,canceled,cancelError,wait);
            Diagnostics::Log("feature_drain",id,cmd,param,drained,drainError,done);
        }
    }
    const bool accepted=ok && GetTickCount64()<deadline &&
        !(stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0) && (write || done>0);
    CloseHandle(ov.hEvent);
    if(accepted && received)*received=done;
    trace.accepted=accepted;trace.done=done;
    if(ok && !accepted)trace.error=(stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0)?ERROR_OPERATION_ABORTED:GetTickCount64()>=deadline?ERROR_TIMEOUT:ERROR_INVALID_DATA;
    return accepted;
}
}
