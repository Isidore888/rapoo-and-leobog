#include "../src/hid_transport.h"
#include <hidclass.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
static int mode=0,calls=0,cancels=0,drains=0;
static bool writing=false;
static HANDLE stop=nullptr;
static OVERLAPPED* pending=nullptr;
static BOOL WINAPI FakeControl(HANDLE,DWORD code,LPVOID input,DWORD inputSize,LPVOID output,DWORD outputSize,LPDWORD done,LPOVERLAPPED ov) {
    ++calls;assert(ov && ov->hEvent && input && inputSize==520);
    assert(code==(writing?IOCTL_HID_SET_FEATURE:IOCTL_HID_GET_FEATURE));
    assert(writing?(!output && !outputSize):(output==input && outputSize==520));
    if(mode==0){*done=writing?0:520;return TRUE;}
    if(mode==1){SetLastError(ERROR_INVALID_FUNCTION);return FALSE;}
    if(mode==5){Sleep(50);*done=520;return TRUE;}
    pending=ov;
    if(mode==3)SetEvent(ov->hEvent);
    if(mode==4)SetEvent(stop);
    SetLastError(ERROR_IO_PENDING);return FALSE;
}
static BOOL WINAPI FakeCancel(HANDLE,LPOVERLAPPED ov){assert(ov==pending);++cancels;SetEvent(ov->hEvent);return TRUE;}
static BOOL WINAPI FakeResult(HANDLE,LPOVERLAPPED ov,LPDWORD done,BOOL wait) {
    assert(ov==pending);
    if(mode==3){assert(!wait);*done=520;return TRUE;}
    assert(wait && cancels);++drains;SetLastError(ERROR_OPERATION_ABORTED);return FALSE;
}
#define DeviceIoControl FakeControl
#define CancelIoEx FakeCancel
#define GetOverlappedResult FakeResult
#include "../src/hid_transport.cpp"
#undef DeviceIoControl
#undef CancelIoEx
#undef GetOverlappedResult
int main() {
    std::filesystem::create_directories("verification/usb-diagnostics-20261014");
    assert(Diagnostics::Init(L"verification/usb-diagnostics-20261014/feature-test"));
    {
    Diagnostics::UsbQuery query(0x84,0,128);
    BYTE data[520]{};data[0]=6;DWORD received=99;
    for(bool write:{false,true}) {
        writing=write;mode=0;assert(Hid::Feature(nullptr,data,520,write,150,nullptr,&received));assert(received==(write?0u:520u));
        mode=1;assert(!Hid::Feature(nullptr,data,520,write,150));
        mode=2;auto start=GetTickCount64();int old=cancels;
        assert(!Hid::Feature(nullptr,data,520,write,30,nullptr,&received));
        assert(cancels==old+1 && drains==cancels && received==0 && GetTickCount64()-start<500);
        mode=3;assert(Hid::Feature(nullptr,data,520,write,150));
        stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);mode=4;old=cancels;
        assert(!Hid::Feature(nullptr,data,520,write,150,stop));assert(cancels==old+1 && drains==cancels);
        int oldCalls=calls;assert(!Hid::Feature(nullptr,data,520,write,150,stop));assert(calls==oldCalls);
        CloseHandle(stop);stop=nullptr;
        mode=5;assert(!Hid::Feature(nullptr,data,520,write,15));
    }
    auto old=calls;assert(!Hid::Feature(nullptr,data,520,false,0));assert(calls==old);
    }
    Diagnostics::Close();
    puts("PASS: overlapped feature buffers, immediate/pending success/error, deadline rejection, timeout/shutdown cancellation and drain; fake IO only");
}
