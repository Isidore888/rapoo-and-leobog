#include <windows.h>
#include <cstdio>
#include <cassert>
#include "../src/hid_transport.h"
static BYTE fakeBank=0, fakeAddress=0, fakeSlot=1, rate=0x84, sleepMinutes=7;
static int writes=0;
static int busyReplies=0,readRequests=0;
static bool failTransfer=false,expectWired=false;
static bool writePending=false;
static BYTE pendingValue=0;
static bool failWriteOutput=false,failWriteAck=false,failSleepReadback=false,mismatchSleep=false;
static int writeBusyReplies=0,featureReads=0;
static bool usbUnavailable=false;
static int usbOpens=0,radioOpens=0;
namespace Hid {
std::vector<Endpoint> TestEnumerate() {
    std::vector<Endpoint> endpoints;
    for(USHORT pid:{USHORT(0x466E),USHORT(0x146E)})for(USHORT usage:{USHORT(2),USHORT(14),USHORT(15)}) {
        Endpoint ep;ep.vid=0x24AE;ep.pid=pid;ep.caps.UsagePage=0xFF00;ep.caps.Usage=usage;
        ep.caps.InputReportByteLength=19;ep.caps.OutputReportByteLength=ep.caps.FeatureReportByteLength=33;
        ep.path=std::wstring(pid==0x466E?L"usb-":L"radio-")+std::to_wstring(usage);endpoints.push_back(ep);
    }
    return endpoints;
}
}
static HANDLE WINAPI TestOpen(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES,DWORD creation,DWORD flags,HANDLE) {
    assert(access==GENERIC_READ && share==(FILE_SHARE_READ|FILE_SHARE_WRITE) && creation==OPEN_EXISTING && flags==FILE_FLAG_OVERLAPPED);
    if(wcscmp(path,L"usb-2")==0){++usbOpens;if(usbUnavailable)return INVALID_HANDLE_VALUE;}
    else {assert(wcscmp(path,L"radio-2")==0);++radioOpens;}
    return CreateEventW(nullptr,FALSE,FALSE,nullptr);
}
static ULONGLONG fakeNow=1000000;
static ULONGLONG WINAPI TestTicks() {return fakeNow;}
static void TestEnter(LPCRITICAL_SECTION);
static void TestLeave(LPCRITICAL_SECTION);
static bool failEvents=false;
static HANDLE WINAPI TestCreateEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name) {
    if(failEvents)return nullptr;
    return ::CreateEventW(attributes,manual,initial,name);
}
namespace Hid {
bool TestTransfer(HANDLE,BYTE* buffer,DWORD size,bool output,DWORD) {
    assert(output && size==33);
    // The device cannot accept a new command before its write ACK is consumed.
    if(writePending)return false;
    fakeBank=buffer[5];fakeAddress=buffer[4];
    assert(buffer[1]==(expectWired?0xFF:0xA5));
    if(buffer[2]==0xA5){
        ++writes;assert(fakeBank==8+fakeSlot*4);
        assert(fakeAddress==0x80 || fakeAddress==0xC2);
        if(failWriteOutput)return false;
        if(!failTransfer){writePending=true;pendingValue=buffer[8];}
        busyReplies=writeBusyReplies;
    } else {assert(buffer[2]==0xA4);++readRequests;}
    return !failTransfer;
}
}
static BOOLEAN WINAPI FakeFeature(HANDLE,PVOID data,ULONG size) {
    ++featureReads;
    assert(size==33);auto reply=(BYTE*)data;ZeroMemory(reply,size);reply[0]=1;
    if(writePending && failWriteAck)return FALSE;
    if(!writePending && fakeAddress==0xC2 && failSleepReadback)return FALSE;
    if(busyReplies>0){--busyReplies;reply[0]=2;return TRUE;}
    if(writePending) {
        if(fakeAddress==0x80)rate=pendingValue;else sleepMinutes=pendingValue;
        writePending=false;return TRUE;
    }
    if(fakeBank==1 && fakeAddress==12)reply[4]=fakeSlot;
    else if(fakeBank==8+fakeSlot*4 && fakeAddress==0x80)reply[4]=rate;
    else if(fakeBank==8+fakeSlot*4 && fakeAddress==0xC2)reply[4]=mismatchSleep?BYTE(sleepMinutes+1):sleepMinutes;
    else assert(false && "read wrong profile");
    return TRUE;
}
#define Transfer TestTransfer
#define HidD_GetFeature FakeFeature
#define Enumerate TestEnumerate
#define CreateFileW TestOpen
#define CreateEventW TestCreateEvent
#define GetTickCount64 TestTicks
#define EnterCriticalSection TestEnter
#define LeaveCriticalSection TestLeave
#include "../src/device_manager.cpp"
#undef Transfer
#undef HidD_GetFeature
#undef Enumerate
#undef CreateFileW
#undef CreateEventW
#undef GetTickCount64
#undef EnterCriticalSection
#undef LeaveCriticalSection
static int ioDepth=0;
static bool injectProfileChange=false;
static bool injectNewerSleep=false;
static void TestEnter(LPCRITICAL_SECTION cs) {::EnterCriticalSection(cs);if(cs==&Device::g_csDevIO)++ioDepth;}
static void TestLeave(LPCRITICAL_SECTION cs) {
    bool change=cs==&Device::g_csDevIO && --ioDepth==0 && injectProfileChange;
    ::LeaveCriticalSection(cs);
    if(change){injectProfileChange=false;Rapoo::DeviceStatus next;next.battery=100;next.profileSlot=2;next.pollingHz=4000;Device::ApplyPassiveStatus(next,false,false);}
    if(cs==&Device::g_csDevIO && ioDepth==0 && injectNewerSleep) {
        injectNewerSleep=false;assert(Device::SetSleepTimeout(13));
    }
}
static LSTATUS WINAPI TestCreate(HKEY,LPCWSTR,DWORD,LPWSTR,DWORD,REGSAM,const LPSECURITY_ATTRIBUTES,PHKEY out,LPDWORD) {*out=reinterpret_cast<HKEY>(1);return ERROR_SUCCESS;}
static LSTATUS WINAPI TestSet(HKEY,LPCWSTR,DWORD,DWORD,const BYTE*,DWORD) {return ERROR_SUCCESS;}
static LSTATUS WINAPI TestClose(HKEY) {return ERROR_SUCCESS;}
#define RegCreateKeyExW TestCreate
#define RegSetValueExW TestSet
#define RegCloseKey TestClose
#include "../src/alert_window.cpp"
#undef RegCreateKeyExW
#undef RegSetValueExW
#undef RegCloseKey
int main() {
    InitializeCriticalSection(&Device::g_csState);InitializeCriticalSection(&Device::g_csDevIO);
    auto now=fakeNow;
    Device::g_currentState.isConnected=true;Device::g_currentState.batteryValid=true;Device::g_currentState.battery=20;
    Device::g_currentState.lastBatteryUpdate=now-Battery::FreshnessMs-1000;
    for(bool wired:{false,true}){Device::g_currentState.isWired=wired;assert(!Device::GetCurrentState().batteryValid);}
    Device::g_currentState.lastBatteryUpdate=now;assert(Device::GetCurrentState().batteryValid);
    Device::g_hControlDev=Device::g_hFeatureDev=reinterpret_cast<HANDLE>(1);
    busyReplies=2;int priorRequests=readRequests;BYTE confirmed=0;
    assert(Device::ReadSetting(1,12,confirmed) && confirmed==1 && readRequests==priorRequests+1);
    assert(Device::RefreshPollingRate());assert(Device::GetCurrentState().pollingHz==2000 && Device::GetCurrentState().sleepMinutes==7 && writes==0);
    // Fake transport only: verify manual settings target the active slot and read back.
    assert(Device::WriteProfileSetting(0x80,1));assert(Device::RefreshPollingRate());assert(Device::GetCurrentState().pollingHz==1000);
    assert(Device::WriteProfileSetting(0xC2,9));Device::g_lastSettingsQuery=0;
    assert(Device::RefreshPollingRate());assert(Device::GetCurrentState().sleepMinutes==9);
    auto guardedGeneration=Device::GetCurrentState().connectionGeneration;
    int guardedWrites=writes,guardedReads=readRequests;
    assert(Device::SetSleepTimeoutChecked(9,guardedGeneration)==Device::SettingResult::Confirmed);
    assert(Device::SetPollingRateChecked(1000,guardedGeneration)==Device::SettingResult::Confirmed);
    assert(writes==guardedWrites && readRequests>guardedReads); // Fresh device read, no duplicate setting writes.
    guardedReads=readRequests;
    assert(Device::SetSleepTimeoutChecked(7,guardedGeneration+1)==Device::SettingResult::Superseded);
    assert(writes==guardedWrites && readRequests==guardedReads);
    fakeSlot=8;int hz=99;assert(!Device::PingDevice(hz));assert(!Device::RefreshPollingRate());assert(Device::GetCurrentState().pollingHz==0 && Device::GetCurrentState().sleepMinutes==0);fakeSlot=1;
    Device::g_controlWired=expectWired=true;assert(Device::ReadSetting(1,12,confirmed)&&confirmed==1);
    Device::g_controlWired=expectWired=false;
    // Feed the real passive-report handler: reconnect is quiet, real DPI change is not.
    Rapoo::DeviceStatus status;status.battery=100;status.pollingHz=2000;status.profileSlot=1;status.dpiX=status.dpiY=1600;
    auto mask=Device::ApplyPassiveStatus(status,false,true);assert(!(mask&Device::CHANGE_DPI)&&Device::GetCurrentState().pollingHz==2000);
    status.dpiX=status.dpiY=800;mask=Device::ApplyPassiveStatus(status,false,false);assert(mask&Device::CHANGE_DPI);
    Device::g_lastSettingsQuery=0;failTransfer=true;priorRequests=readRequests;
    Device::RefreshPollingRate();assert(readRequests==priorRequests+1&&Device::g_controlBlocked&&Device::GetCurrentState().pollingHz==2000);
    Device::g_lastSettingsQuery=0;Device::RefreshPollingRate();assert(readRequests==priorRequests+1);
    failTransfer=false;Device::g_controlBlocked=false;Device::g_lastSettingsQuery=0;
    Device::RefreshPollingRate();assert(Device::GetCurrentState().sleepMinutes==9);
    // A failed automatic read must not permanently disable deliberate settings.
    failTransfer=true;assert(!Device::ReadSetting(1,12,confirmed));failTransfer=false;
    auto result=Device::SetSleepTimeoutChecked(8);
    assert(result==Device::SettingResult::Confirmed && !Device::g_controlBlocked && sleepMinutes==8);
    // Both prefixes/settings await busy write ACKs before their first readback.
    for(bool wired:{false,true}) {
        Device::g_controlWired=expectWired=wired;writeBusyReplies=3;
        int priorWrites=writes,priorFeatures=featureReads;
        assert(Device::SetPollingRateChecked(2000)==Device::SettingResult::Confirmed);
        assert(writes==priorWrites+1 && featureReads>=priorFeatures+7 && !writePending);
        assert(Device::SetSleepTimeoutChecked(9)==Device::SettingResult::Confirmed);
    }
    Device::g_controlWired=expectWired=false;writeBusyReplies=0;
    // Missing write ACK: invalidate stale value, never send a read over the write.
    failWriteAck=true;priorRequests=readRequests;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::WriteAckFailed);
    assert(readRequests==priorRequests+1 && writePending && Device::GetCurrentState().sleepMinutes==0);
    int priorWrites=writes,priorFeatures=featureReads;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::RetryLater);
    assert(writes==priorWrites && readRequests==priorRequests+1 && featureReads==priorFeatures);
    // Even after cooldown, unresolved response is polled without another command.
    fakeNow+=2001;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::RecoveryFailed);
    assert(writes==priorWrites && readRequests==priorRequests+1);
    failWriteAck=false;fakeNow+=2001;
    assert(Device::SetSleepTimeoutChecked(9)==Device::SettingResult::Confirmed && !writePending);
    // Distinguish output failure from accepted write whose readback failed.
    failWriteOutput=true;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::WriteFailed);
    assert(Device::GetCurrentState().sleepMinutes==0 && !writePending && sleepMinutes==9);
    failWriteOutput=false;fakeNow+=2001;
    assert(Device::SetSleepTimeoutChecked(9)==Device::SettingResult::Confirmed);
    failSleepReadback=true;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::ReadbackFailed);
    assert(sleepMinutes==5 && Device::GetCurrentState().sleepMinutes==0 && !writePending);
    failSleepReadback=false;fakeNow+=2001;
    assert(Device::SetSleepTimeoutChecked(9)==Device::SettingResult::Confirmed);
    mismatchSleep=true;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::ReadbackMismatch);
    assert(Device::GetCurrentState().sleepMinutes==0);
    mismatchSleep=false;assert(Device::SetSleepTimeoutChecked(9)==Device::SettingResult::Confirmed);
    fakeSlot=2;priorWrites=writes;
    Device::g_currentState.profileSlot=1;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::ProfileChanged && writes==priorWrites);
    fakeSlot=1;failTransfer=true;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::ProfileReadFailed && writes==priorWrites);
    failTransfer=false;fakeNow+=2001;
    assert(Device::SetSleepTimeoutChecked(9)==Device::SettingResult::Confirmed);
    Device::g_currentState.isConnected=false;priorRequests=readRequests;
    assert(Device::SetSleepTimeoutChecked(5)==Device::SettingResult::Disconnected && readRequests==priorRequests);
    Device::g_currentState.isConnected=true;
    assert(Device::SetPollingRateChecked(999)==Device::SettingResult::InvalidValue && readRequests==priorRequests);
    assert(wcscmp(Device::SettingResultText(Device::SettingResult::WriteAckFailed),L"写入应答失败，结果未确认")==0);
    assert(Device::SettingResultText(Device::SettingResult::Confirmed)[0]==0);
    priorRequests=readRequests;Device::RefreshPollingRate();assert(readRequests==priorRequests);
    status.profileSlot=2;Device::ApplyPassiveStatus(status,false,false);assert(Device::GetCurrentState().sleepMinutes==0);
    // A switch inside the cooldown leaves a pending read, serviced once due.
    fakeSlot=2;sleepMinutes=11;priorRequests=readRequests;Device::RefreshSleepTimeout();
    assert(readRequests==priorRequests && Device::g_sleepRefreshPending);
    Device::g_lastSettingsQuery=fakeNow-60001;Device::RefreshSleepTimeout(false);
    assert(Device::GetCurrentState().sleepMinutes==11 && readRequests==priorRequests+3 && !Device::g_sleepRefreshPending);
    Device::RefreshSleepTimeout(false);assert(readRequests==priorRequests+3);
    // Switch between confirmed I/O and publication: never publish into the new slot.
    for(bool sleep:{true,false}) {
        fakeSlot=1;status.profileSlot=1;Device::ApplyPassiveStatus(status,false,false);
        injectProfileChange=true;
        assert(!(sleep?Device::SetSleepTimeout(9):Device::SetPollingRate(1000)));
        auto current=Device::GetCurrentState();assert(current.profileSlot==2 && current.sleepMinutes==0 && current.pollingHz==4000);
    }
    fakeSlot=1;status.profileSlot=1;Device::ApplyPassiveStatus(status,false,false);
    Device::g_lastSettingsQuery=0;injectProfileChange=true;Device::RefreshSleepTimeout();
    assert(Device::GetCurrentState().profileSlot==2 && Device::GetCurrentState().sleepMinutes==0 && Device::g_sleepRefreshPending);
    auto oldStamp=Device::CurrentProfileStamp();status.profileSlot=1;Device::ApplyPassiveStatus(status,false,false);status.profileSlot=2;Device::ApplyPassiveStatus(status,false,false);
    assert(!Device::PublishSetting(oldStamp,99,true)&&Device::GetCurrentState().sleepMinutes==0);
    fakeSlot=1;status.profileSlot=1;Device::ApplyPassiveStatus(status,false,false);
    Device::g_lastSettingsQuery=0;sleepMinutes=7;injectNewerSleep=true;Device::RefreshSleepTimeout();
    assert(Device::GetCurrentState().sleepMinutes==13);
    injectNewerSleep=true;assert(!Device::SetSleepTimeout(9));assert(Device::GetCurrentState().sleepMinutes==13);
    WCHAR path[MAX_PATH]{},control[MAX_PATH]{},feature[MAX_PATH]{},model[64]{};bool wired=false;
    usbUnavailable=true;HANDLE opened=Device::OpenRapooStatus(path,control,feature,model,64,&wired);
    assert(opened!=INVALID_HANDLE_VALUE && !wired && wcscmp(path,L"radio-2")==0 && wcscmp(control,L"radio-14")==0 && wcscmp(feature,L"radio-15")==0);CloseHandle(opened);
    assert(Device::KeepCurrentStatus(L"radio-2"));
    usbUnavailable=false;opened=Device::OpenRapooStatus(path,control,feature,model,64,&wired);
    assert(opened!=INVALID_HANDLE_VALUE && wired && wcscmp(path,L"usb-2")==0);CloseHandle(opened);
    assert(!Device::KeepCurrentStatus(L"radio-2") && usbOpens>0 && radioOpens>0);
    Device::g_hRefreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);Device::ForceRefresh();assert(WaitForSingleObject(Device::g_hRefreshEvent,0)==WAIT_OBJECT_0);CloseHandle(Device::g_hRefreshEvent);Device::g_hRefreshEvent=nullptr;
    Device::g_hControlDev=Device::g_hFeatureDev=INVALID_HANDLE_VALUE;
    DeleteCriticalSection(&Device::g_csDevIO);DeleteCriticalSection(&Device::g_csState);
    failEvents=true;assert(!Device::Start(nullptr,nullptr));
    assert(!Device::g_started && !Device::g_hStopEvent && !Device::g_hDevChangeEvent && !Device::g_hRefreshEvent && !Device::g_hWorkerThread);
    Device::Stop();Device::Stop();failEvents=false;
    Alert::configs[0]={};Alert::configs[1]={};Alert::trackers[1].warned=true;
    Alert::pending.push_back({1,L"LEOBOG HI98C Pro",20,false,false});
    AlertPolicy::Config changed[2];changed[0].enabled=false;assert(Alert::SaveConfig(changed));
    assert(Alert::pending.size()==1 && Alert::trackers[1].warned);
    assert(Alert::trackers[1].Evaluate(Alert::configs[1],true,true,20,false)==0);
    assert(Alert::SaveConfig(changed));assert(Alert::pending.size()==1 && Alert::trackers[1].warned);
    changed[1].warning=30;assert(Alert::SaveConfig(changed));assert(Alert::pending.empty() && !Alert::trackers[1].warned);
    Alert::configs[0]={};Alert::configs[1]={};Alert::trackers[0]={};Alert::trackers[1]={};
    Device::State mouse;mouse.isConnected=mouse.batteryValid=true;mouse.battery=20;
    Keyboard::State keyboard;keyboard.isConnected=keyboard.batteryValid=true;keyboard.battery=20;
    Alert::s_isShown=true;Alert::activeDevice=0;Alert::s_isCritical=false;Alert::s_battery=20;Alert::s_isWired=false;Alert::trackers[0].warned=true;
    Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1);
    keyboard.battery=19;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.front().battery==19);
    keyboard.battery=26;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.empty());
    mouse.battery=10;Alert::CheckBattery(mouse,keyboard);
    assert(Alert::s_isShown && Alert::activeDevice==0 && Alert::s_isCritical && Alert::s_battery==10 && Alert::pending.empty());
    mouse.battery=26;Alert::CheckBattery(mouse,keyboard);assert(!Alert::s_isShown && Alert::activeDevice==-1);
    // Cancel both recovered devices before selecting any queued card.
    Alert::s_isShown=true;Alert::activeDevice=0;Alert::pending.push_back({1,L"keyboard",20,false,false});
    Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.empty() && !Alert::s_isShown && Alert::activeDevice==-1);
    // A canceled notification which never became visible does not consume history.
    mouse.battery=20;Alert::trackers[0]={};Alert::trackers[0].warned=true;
    Alert::s_isShown=true;Alert::activeDevice=0;keyboard.battery=20;Alert::trackers[1]={};
    Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1 && Alert::trackers[1].warned);
    keyboard.battery=21;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.empty() && !Alert::trackers[1].warned);
    keyboard.battery=20;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1);
    keyboard.battery=10;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.front().critical);
    keyboard.battery=15;Alert::CheckBattery(mouse,keyboard);assert(!Alert::pending.front().critical && !Alert::trackers[1].critical);
    // Simulate that queued warning being displayed/dismissed, without a HWND.
    Alert::pending.clear();keyboard.battery=10;Alert::CheckBattery(mouse,keyboard);
    assert(Alert::pending.size()==1 && Alert::pending.front().critical);
    keyboard.battery=15;Alert::CheckBattery(mouse,keyboard);
    assert(Alert::pending.empty() && Alert::trackers[1].warned && !Alert::trackers[1].critical);
    keyboard.battery=10;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1 && Alert::pending.front().critical);
    Alert::pending.clear();Alert::configs[1]={true,100,99};Alert::trackers[1]={true,true,99};
    keyboard.battery=100;Alert::CheckBattery(mouse,keyboard);
    assert(Alert::pending.size()==1 && Alert::pending.front().consumedWarning);
    keyboard.isConnected=false;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.empty() && !Alert::trackers[1].warned);
    keyboard.isConnected=true;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1);
    Alert::pending.clear();Alert::trackers[1]={true,false,100};keyboard.battery=99;
    Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1 && Alert::pending.front().critical && !Alert::pending.front().consumedWarning);
    keyboard.battery=100;Alert::CheckBattery(mouse,keyboard);
    assert(Alert::pending.size()==1 && !Alert::pending.front().critical && Alert::pending.front().consumedWarning);
    Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1);
    keyboard.isConnected=false;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.empty() && !Alert::trackers[1].warned);
    keyboard.isConnected=true;Alert::CheckBattery(mouse,keyboard);assert(Alert::pending.size()==1);
    puts("PASS: write ACK sequencing USB/radio, bounded manual recovery, output/ACK/readback/mismatch/profile failures, no false confirmation or automatic retry");
    puts("PASS: deferred profile refresh, read/write publication races, USB-open fallback, recovered queues, live severity upgrade; simulated HID/registry, no windows");
    puts("PASS: real state freshness USB/radio, active profile query/write/readback, invalid slot, async refresh, unchanged alert queue/history; no hardware/registry writes");
}
