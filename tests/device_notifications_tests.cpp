#include "../src/device_notifications.h"
#include <cassert>
#include <cstdio>
#include <vector>
static std::vector<BYTE> Message(const wchar_t* path) {
    std::vector<BYTE> data(offsetof(DEV_BROADCAST_DEVICEINTERFACE_W,dbcc_name)+(wcslen(path)+1)*sizeof(wchar_t));
    auto* info=reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(data.data());
    info->dbcc_size=DWORD(data.size());info->dbcc_devicetype=DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&info->dbcc_classguid);wcscpy(info->dbcc_name,path);return data;
}
int main() {
    using namespace DeviceNotifications;
    for(auto event:{DBT_DEVICEARRIVAL,DBT_DEVICEREMOVECOMPLETE}) {
        for(auto path:{L"\\\\?\\hid#vid_258a&pid_010c&mi_00#test",L"\\\\?\\HID#VID_3554&PID_FA09#test"}) {
            auto data=Message(path);assert(Targets(event,reinterpret_cast<LPARAM>(data.data()))==Keyboard);
        }
        for(auto path:{L"\\\\?\\HID#VID_24AE&PID_466E&MI_01#test",L"\\\\?\\HID#VID_24AE&PID_146E#test"}) {
            auto data=Message(path);assert(Targets(event,reinterpret_cast<LPARAM>(data.data()))==Mouse);
        }
    }
    for(auto path:{L"\\\\?\\USB#VID_258A&PID_010C#test",L"\\\\?\\HID#VID_258A&PID_010C0#test",L"\\\\?\\HID#VID_24AE&PID_9999#test",L"\\\\?\\HID#VID_1234&PID_010C#test"}) {
        auto data=Message(path);assert(!Targets(DBT_DEVICEARRIVAL,reinterpret_cast<LPARAM>(data.data())));
    }
    auto data=Message(L"\\\\?\\HID#VID_258A&PID_010C#test");auto address=reinterpret_cast<LPARAM>(data.data());
    assert(!Targets(DBT_DEVNODES_CHANGED,address) && !Targets(DBT_DEVICEARRIVAL,0));
    auto* info=reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(data.data());
    info->dbcc_classguid={};assert(!Targets(DBT_DEVICEARRIVAL,address));HidD_GetHidGuid(&info->dbcc_classguid);
    info->dbcc_size=sizeof(DEV_BROADCAST_HDR);assert(!Targets(DBT_DEVICEARRIVAL,address));info->dbcc_size=DWORD(data.size());
    data[data.size()-2]=1;assert(!Targets(DBT_DEVICEARRIVAL,address));
    puts("PASS: exact mouse/keyboard HID arrival/removal, unrelated/global events, malformed size/GUID/path ignored; no device access");
}
