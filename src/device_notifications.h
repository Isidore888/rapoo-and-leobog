#pragma once
#include <windows.h>
#include <dbt.h>
#include <algorithm>
#include <cstddef>
#include <cwchar>
extern "C" {
#include <hidsdi.h>
}
namespace DeviceNotifications {
constexpr unsigned Mouse=1,Keyboard=2;
inline unsigned Targets(WPARAM event,LPARAM data) {
    if((event!=DBT_DEVICEARRIVAL && event!=DBT_DEVICEREMOVECOMPLETE) || !data)return 0;
    const auto* header=reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
    constexpr auto offset=offsetof(DEV_BROADCAST_DEVICEINTERFACE_W,dbcc_name);
    if(header->dbch_devicetype!=DBT_DEVTYP_DEVICEINTERFACE || header->dbch_size<offset+sizeof(wchar_t))return 0;
    const auto* info=reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(data);
    GUID hid{};HidD_GetHidGuid(&hid);if(!IsEqualGUID(info->dbcc_classguid,hid))return 0;
    const auto count=(info->dbcc_size-offset)/sizeof(wchar_t);
    const auto* end=std::find(info->dbcc_name,info->dbcc_name+count,L'\0');
    if(end==info->dbcc_name+count)return 0;
    const struct {const wchar_t* id;unsigned target;} devices[]={
        {L"\\\\?\\HID#VID_24AE&PID_466E",Mouse},{L"\\\\?\\HID#VID_24AE&PID_146E",Mouse},
        {L"\\\\?\\HID#VID_258A&PID_010C",Keyboard},{L"\\\\?\\HID#VID_3554&PID_FA09",Keyboard}};
    for(const auto& device:devices) {
        const auto length=wcslen(device.id);
        if(size_t(end-info->dbcc_name)>length && !_wcsnicmp(info->dbcc_name,device.id,length) &&
            (info->dbcc_name[length]==L'&' || info->dbcc_name[length]==L'#'))
            return device.target;
    }
    return 0;
}
}
