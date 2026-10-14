#include "device_manager.h"
#include "hid_transport.h"
extern "C" {
#include <hidsdi.h>
}
#include <setupapi.h>
#include <strsafe.h>
#include <algorithm>
#include <atomic>
#ifdef RAPOO_DIAGNOSTIC
#include <cstdio>
#endif

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace Device {

static State g_currentState;
static StateCallback g_callback = nullptr;
static bool g_started = false;

static HANDLE g_hWorkerThread = NULL;
static HANDLE g_hStopEvent = NULL;
static HANDLE g_hDevChangeEvent = NULL;
static HANDLE g_hRefreshEvent = NULL;

static CRITICAL_SECTION g_csDevIO;
static CRITICAL_SECTION g_csState;
static HANDLE g_hControlDev = INVALID_HANDLE_VALUE;
static HANDLE g_hFeatureDev = INVALID_HANDLE_VALUE;
static bool g_controlWired = false;
// A failed transaction stops automatic requests. Explicit settings may retry.
// Do not repeatedly occupy a device also used by the official driver.
static bool g_controlBlocked = false;
static ULONGLONG g_lastManualFailure = 0; // g_csDevIO; bounds retries after failure
static bool g_lastCommandSent = false; // distinguishes output failure from missing ACK
static bool g_replyPending = false; // finish an interrupted response before manual recovery
static bool g_passivePolling = false;
static bool g_allowSettingsQueries = true;
static ULONGLONG g_lastSettingsQuery = 0;
// Protected by g_csState; a connection/profile change invalidates in-flight results.
static ULONGLONG g_profileRevision = 0;
static bool g_sleepRefreshPending = false;
static std::atomic<void (*)(bool)> g_controlScheduler{nullptr};
static ULONGLONG g_settingsSerial=0; // g_csDevIO
static ULONGLONG g_sleepPublishedSerial=0,g_pollingPublishedSerial=0; // g_csState
struct ProfileStamp { int slot=-1; ULONGLONG revision=0,serial=0; };
static ProfileStamp CurrentProfileStamp() {
    EnterCriticalSection(&g_csState);
    ProfileStamp stamp{g_currentState.profileSlot,g_profileRevision};
    LeaveCriticalSection(&g_csState);return stamp;
}
static bool PublishSetting(const ProfileStamp& stamp, int value, bool sleep) {
    EnterCriticalSection(&g_csState);
    bool current=g_currentState.isConnected && stamp.revision==g_profileRevision &&
        (g_currentState.profileSlot<0 || stamp.slot==g_currentState.profileSlot);
    auto& published=sleep?g_sleepPublishedSerial:g_pollingPublishedSerial;
    current=current && stamp.serial>=published;
    bool changed=false;
    if(current) {
        published=stamp.serial;
        int& setting=sleep?g_currentState.sleepMinutes:g_currentState.pollingHz;
        changed=setting!=value;setting=value;
        if(sleep)g_sleepRefreshPending=false;
    }
    State copy=g_currentState;
    LeaveCriticalSection(&g_csState);
    if(changed && g_callback)g_callback(copy,sleep?CHANGE_SLEEP:CHANGE_POLLING);
    return current;
}

// Reads and writes both return ACK 1; ACK 2 means keep waiting for this command.
// Consume the write response before issuing a read, as the official A Hub SDK does.
static bool WaitSettingReply(BYTE expectedLen, BYTE* value=nullptr, bool drain=false) {
    for(int poll=0;poll<6;++poll) {
        Sleep(poll?100:40);
        if(g_hStopEvent && WaitForSingleObject(g_hStopEvent,0)==WAIT_OBJECT_0)return false;
        BYTE reply[33]{};reply[0]=Rapoo::REPORT_ID_FEATURE;
        if(!HidD_GetFeature(g_hFeatureDev,reply,sizeof(reply))) {
#ifdef RAPOO_DIAGNOSTIC
            fprintf(stderr,"feature failed error=%lu\n",GetLastError());
#endif
            return false;
        }
#ifdef RAPOO_DIAGNOSTIC
        fprintf(stderr,"response len=%u ack=%02X/%02X data=%02X/%02X\n",expectedLen,reply[0],reply[1],reply[4],reply[5]);
#endif
        if(Rapoo::ParseFeatureRead(reply,sizeof(reply),expectedLen,value)){g_replyPending=false;return true;}
        if(reply[0]!=2 && !(reply[0]==Rapoo::REPORT_ID_FEATURE && reply[1]==2)) {
            g_replyPending=false;return drain; // terminal error ACK also finishes the old command
        }
    }
    return false;
}

static bool SendRawCommand(BYTE bank, BYTE addr, const BYTE* pData, BYTE len) {
    EnterCriticalSection(&g_csDevIO);
    g_lastCommandSent=false;
    if (g_hControlDev == INVALID_HANDLE_VALUE || g_hFeatureDev == INVALID_HANDLE_VALUE || g_controlBlocked ||
        (g_hStopEvent && WaitForSingleObject(g_hStopEvent,0)==WAIT_OBJECT_0)) {
        LeaveCriticalSection(&g_csDevIO);
        return false;
    }

    BYTE buf[33] = {0};
    DWORD pktSize = Rapoo::BuildWriteCommand(bank, addr, pData, len, buf, sizeof(buf), g_controlWired);

    g_replyPending=true;
    g_lastCommandSent=Hid::Transfer(g_hControlDev,buf,pktSize,true,800);
    bool success=g_lastCommandSent && WaitSettingReply(0);
    if(!success)g_controlBlocked=true;
    LeaveCriticalSection(&g_csDevIO);
    return success;
}

static bool ReadSetting(BYTE bank, BYTE addr, BYTE& value) {
#ifdef RAPOO_DIAGNOSTIC
    fprintf(stderr,"request bank=%02X addr=%02X control=%d feature=%d\n",bank,addr,g_hControlDev!=INVALID_HANDLE_VALUE,g_hFeatureDev!=INVALID_HANDLE_VALUE);
#endif
    EnterCriticalSection(&g_csDevIO);
    bool success = false;
    if (!g_controlBlocked && g_hControlDev != INVALID_HANDLE_VALUE && g_hFeatureDev != INVALID_HANDLE_VALUE &&
        !(g_hStopEvent && WaitForSingleObject(g_hStopEvent,0)==WAIT_OBJECT_0)) {
        {
            BYTE request[33]{};
            Rapoo::BuildReadCommand(bank,addr,1,request,sizeof(request),g_controlWired);
            g_replyPending=true;
            if (!Hid::Transfer(g_hControlDev,request,sizeof(request),true,800)) {
#ifdef RAPOO_DIAGNOSTIC
                fprintf(stderr,"write failed error=%lu\n",GetLastError());
#endif
                g_controlBlocked=true;
                LeaveCriticalSection(&g_csDevIO);return false;
            }
            success=WaitSettingReply(1,&value);
            if(!success)g_controlBlocked=true;
        }
    }
    LeaveCriticalSection(&g_csDevIO);
    return success;
}
static bool ReadProfileSetting(BYTE addr, BYTE& value, ProfileStamp* result=nullptr) {
    EnterCriticalSection(&g_csDevIO);
    auto stamp=CurrentProfileStamp();
    stamp.serial=++g_settingsSerial;
    bool success=false;
    for(int retry=0;retry<2 && !success;++retry) {
        BYTE slot=0,after=0,bank=0;
        if(!ReadSetting(1,0x0C,slot) || !Rapoo::ProfileBank(slot,bank))continue;
        if(stamp.slot>=0 && slot!=stamp.slot)continue;
        success=ReadSetting(bank,addr,value) && ReadSetting(1,0x0C,after) && slot==after;
        if(success)stamp.slot=slot;
    }
    if(result)*result=stamp;
    LeaveCriticalSection(&g_csDevIO);return success;
}
static bool WriteProfileSetting(BYTE addr, BYTE value, ProfileStamp* result=nullptr, SettingResult* outcome=nullptr,ULONGLONG expectedGeneration=~ULONGLONG(0)) {
    EnterCriticalSection(&g_csDevIO);
    BYTE slot=0,bank=0,confirmed=0,after=0;
    auto stamp=CurrentProfileStamp();
    stamp.serial=++g_settingsSerial;
    SettingResult status=SettingResult::Confirmed;
    bool writeAttempted=false;
    EnterCriticalSection(&g_csState);bool connected=g_currentState.isConnected;LeaveCriticalSection(&g_csState);
    if((g_hStopEvent && WaitForSingleObject(g_hStopEvent,0)==WAIT_OBJECT_0) ||
       (expectedGeneration!=~ULONGLONG(0) && stamp.revision!=expectedGeneration))status=SettingResult::Superseded;
    else if(!connected)status=SettingResult::Disconnected;
    else if(g_hControlDev==INVALID_HANDLE_VALUE || g_hFeatureDev==INVALID_HANDLE_VALUE)status=SettingResult::InterfaceUnavailable;
    else if(g_controlBlocked && g_lastManualFailure && GetTickCount64()-g_lastManualFailure<2000)status=SettingResult::RetryLater;
    else {
        // A deliberate user action gets one bounded attempt; background reads
        // remain blocked after a failure. No reopen, unlock or write resending.
        if(g_controlBlocked && g_replyPending && !WaitSettingReply(0,nullptr,true))status=SettingResult::RecoveryFailed;
        else g_controlBlocked=false;
        if(status==SettingResult::Confirmed) {
            if(!ReadSetting(1,0x0C,slot) || !Rapoo::ProfileBank(slot,bank))status=SettingResult::ProfileReadFailed;
            else if(stamp.slot>=0 && slot!=stamp.slot)status=SettingResult::ProfileChanged;
            else {
                stamp.slot=slot;
                bool same=false;
                if(expectedGeneration!=~ULONGLONG(0)) {
                    if(!ReadSetting(bank,addr,confirmed) || !ReadSetting(1,0x0C,after))status=SettingResult::CurrentReadFailed;
                    else if(slot!=after || CurrentProfileStamp().revision!=stamp.revision)status=SettingResult::ProfileChanged;
                    else same=confirmed==value;
                }
                if(status==SettingResult::Confirmed && !same) {
                    writeAttempted=true;
                    if(!SendRawCommand(bank,addr,&value,1))status=g_lastCommandSent?SettingResult::WriteAckFailed:SettingResult::WriteFailed;
                    else if(!ReadSetting(bank,addr,confirmed))status=SettingResult::ReadbackFailed;
                    else if(confirmed!=value)status=SettingResult::ReadbackMismatch;
                    else if(!ReadSetting(1,0x0C,after))status=SettingResult::ReadbackFailed;
                    else if(slot!=after)status=SettingResult::ProfileChanged;
                }
            }
        }
        if(g_controlBlocked)g_lastManualFailure=GetTickCount64();else g_lastManualFailure=0;
    }
    bool success=status==SettingResult::Confirmed;
    if(result)*result=stamp;
    if(outcome)*outcome=status;
    LeaveCriticalSection(&g_csDevIO);
    // A command may have reached the device even when confirmation failed.
    // Do not keep presenting the old setting as a confirmed current value.
    if(writeAttempted && !success)PublishSetting(stamp,0,addr==Rapoo::ADDR_SLEEP_TIMEOUT);
    return success;
}
static bool PingDevice(int& outHz, ProfileStamp* stamp=nullptr) {
    BYTE value=0;
    if(!ReadProfileSetting(Rapoo::ADDR_POLLING_HZ,value,stamp)) return false;
    outHz=Rapoo::CodeToPollingHz(value);
    return outHz>0;
}
static void RefreshSleepTimeout(bool request=true) {
    EnterCriticalSection(&g_csDevIO);
    EnterCriticalSection(&g_csState);
    if(request)g_sleepRefreshPending=true;
    bool pending=g_sleepRefreshPending && g_currentState.isConnected;
    if(!g_allowSettingsQueries || g_controlBlocked)g_sleepRefreshPending=false;
    LeaveCriticalSection(&g_csState);
    auto now=GetTickCount64();
    if(!pending || !g_allowSettingsQueries || g_controlBlocked ||
       (g_lastSettingsQuery && now-g_lastSettingsQuery<60000)) {
        LeaveCriticalSection(&g_csDevIO);return;
    }
    g_lastSettingsQuery=now;
    EnterCriticalSection(&g_csState);g_sleepRefreshPending=false;LeaveCriticalSection(&g_csState);
    BYTE value=0;
    ProfileStamp stamp;
    if(!ReadProfileSetting(Rapoo::ADDR_SLEEP_TIMEOUT,value,&stamp) || value<2 || value>120)value=0;
    LeaveCriticalSection(&g_csDevIO);
    PublishSetting(stamp,value,true);
}
void SetControlScheduler(void (*schedule)(bool)){g_controlScheduler.store(schedule);}
void RunControlRefresh(bool request){RefreshSleepTimeout(request);}
static void ScheduleSleepRefresh(bool request=true) {
    auto scheduler=g_controlScheduler.load();
    if(!scheduler){RefreshSleepTimeout(request);return;}
    EnterCriticalSection(&g_csState);
    if(request)g_sleepRefreshPending=true;
    bool pending=g_sleepRefreshPending && g_currentState.isConnected;
    LeaveCriticalSection(&g_csState);
    if(pending)scheduler(false);
}

static DWORD ApplyPassiveStatus(const Rapoo::DeviceStatus& status, bool wired, bool initial) {
    EnterCriticalSection(&g_csState);
    DWORD mask=0;
    if(!g_currentState.isConnected){g_currentState.isConnected=true;++g_profileRevision;mask|=CHANGE_CONNECTED;}
    bool effectiveWired=wired || status.isWired;
    if(g_currentState.isWired!=effectiveWired){g_currentState.isWired=effectiveWired;++g_profileRevision;mask|=CHANGE_CONNECTED;}
    if(status.dpiLevel!=g_currentState.dpiLevel || status.dpiX!=g_currentState.dpiX || status.dpiY!=g_currentState.dpiY) {
        g_currentState.dpiLevel=status.dpiLevel;g_currentState.dpiX=status.dpiX;g_currentState.dpiY=status.dpiY;
        if(!initial)mask|=CHANGE_DPI;
    }
    if(status.profileSlot>=0 || status.pollingHz>0) {
        g_passivePolling=true;
        if(g_currentState.pollingHz!=status.pollingHz){g_currentState.pollingHz=status.pollingHz;mask|=CHANGE_POLLING;}
    }
    if(g_currentState.profileSlot!=status.profileSlot) {
        ++g_profileRevision;g_sleepRefreshPending=true;
        g_currentState.profileSlot=status.profileSlot;
        if(g_currentState.sleepMinutes){g_currentState.sleepMinutes=0;mask|=CHANGE_SLEEP;}
    }
    if(!g_currentState.batteryValid || g_currentState.battery!=status.battery || g_currentState.isCharging!=status.isCharging)mask|=CHANGE_BATTERY;
    g_currentState.battery=status.battery;g_currentState.isCharging=status.isCharging;
    g_currentState.batteryValid=true;g_currentState.lastBatteryUpdate=GetTickCount64();
    LeaveCriticalSection(&g_csState);return mask;
}

// Only identities observed on the user's VT3S MAX Master V2 are accepted.
static bool FindRapooEndpoints(WCHAR* pathStatus, WCHAR* pathControl, WCHAR* pathFeature,
                               WCHAR* outModel, int maxModelLen, bool* outIsWired, USHORT excludePid=0) {
    auto endpoints = Hid::Enumerate();
    for (USHORT pid : {USHORT(0x466E), USHORT(0x146E)}) {
        if(pid==excludePid)continue;
        pathStatus[0] = pathControl[0] = pathFeature[0] = 0;
        for (const auto& ep : endpoints) {
            if (ep.vid != 0x24AE || ep.pid != pid || ep.caps.UsagePage != 0xFF00 || ep.path.size() >= MAX_PATH) continue;
            const auto& c = ep.caps;
            if (c.Usage == 2 && c.InputReportByteLength == 19) StringCchCopyW(pathStatus,MAX_PATH,ep.path.c_str());
            if (c.Usage == 14 && c.OutputReportByteLength == 33) StringCchCopyW(pathControl,MAX_PATH,ep.path.c_str());
            if (c.Usage == 15 && c.FeatureReportByteLength == 33) StringCchCopyW(pathFeature,MAX_PATH,ep.path.c_str());
        }
        if (pathStatus[0] && pathControl[0] && pathFeature[0]) {
            StringCchCopyW(outModel,maxModelLen,L"雷柏 VT3S MAX 大师版 V2");
            *outIsWired = pid == 0x466E;
            return true;
        }
    }
    return false;
}

static HANDLE OpenStatusPath(const WCHAR* path) {
    return CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                       OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
}
static HANDLE OpenRapooStatus(WCHAR* status,WCHAR* control,WCHAR* feature,
                              WCHAR* model,int modelLength,bool* wired) {
    if(!FindRapooEndpoints(status,control,feature,model,modelLength,wired))return INVALID_HANDLE_VALUE;
    HANDLE handle=OpenStatusPath(status);
    if(handle==INVALID_HANDLE_VALUE && *wired &&
       FindRapooEndpoints(status,control,feature,model,modelLength,wired,0x466E))
        handle=OpenStatusPath(status);
    return handle;
}
static bool KeepCurrentStatus(const WCHAR* current) {
    WCHAR status[MAX_PATH]{},control[MAX_PATH]{},feature[MAX_PATH]{},model[64]{};bool wired=false;
    if(!FindRapooEndpoints(status,control,feature,model,64,&wired))return false;
    if(wcscmp(status,current)==0)return true;
    // An inaccessible USB interface must not repeatedly displace a working receiver.
    HANDLE candidate=OpenRapooStatus(status,control,feature,model,64,&wired);
    if(candidate==INVALID_HANDLE_VALUE)return false;
    CloseHandle(candidate);return wcscmp(status,current)==0;
}

static DWORD WINAPI HidWorkerThread(LPVOID lpParam) {
    WCHAR pathStatus[MAX_PATH] = {0};
    WCHAR pathControl[MAX_PATH] = {0};
    WCHAR pathFeature[MAX_PATH] = {0};
    WCHAR modelBuf[64] = {0};
    bool isWired = false;

    while (WaitForSingleObject(g_hStopEvent, 200) == WAIT_TIMEOUT) {
        HANDLE hStatus=OpenRapooStatus(pathStatus,pathControl,pathFeature,modelBuf,64,&isWired);

        if (hStatus == INVALID_HANDLE_VALUE) {
            EnterCriticalSection(&g_csState);
            bool wasConn = g_currentState.isConnected;
            g_currentState.isConnected = false;
            g_currentState.isCharging = false;
            State copySt = g_currentState;
            LeaveCriticalSection(&g_csState);
            if (wasConn && g_callback) g_callback(copySt, CHANGE_CONNECTED | CHANGE_BATTERY);
            Sleep(1000);
            continue;
        }

        EnterCriticalSection(&g_csDevIO);
        g_controlWired=isWired;g_controlBlocked=false;g_replyPending=false;g_lastManualFailure=0;g_lastSettingsQuery=0;
        if (pathControl[0]) {
            g_hControlDev = CreateFileW(
                pathControl,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                NULL
            );
            if (g_hControlDev == INVALID_HANDLE_VALUE) {
                g_hControlDev = CreateFileW(
                    pathControl,
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    NULL
                );
            }
        }
        if (pathFeature[0]) {
            g_hFeatureDev = CreateFileW(
                pathFeature,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );
            if (g_hFeatureDev == INVALID_HANDLE_VALUE) {
                g_hFeatureDev = CreateFileW(
                    pathFeature,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL
                );
            }
            if (g_hFeatureDev == INVALID_HANDLE_VALUE) {
                g_hFeatureDev = CreateFileW(
                    pathFeature,
                    0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL
                );
            }
        }
        EnterCriticalSection(&g_csState);
        StringCchCopyW(g_currentState.modelName, ARRAYSIZE(g_currentState.modelName), modelBuf);
        g_currentState.isWired = isWired;
        g_currentState.isCharging = false;
        g_currentState.batteryValid = false;
        g_currentState.pollingHz = 0;
        g_currentState.sleepMinutes = 0;
        g_currentState.profileSlot = -1;
        ++g_profileRevision;g_sleepRefreshPending=false;
        g_passivePolling=false;
        g_currentState.isConnected = true;
        State connSt = g_currentState;
        LeaveCriticalSection(&g_csState);
        LeaveCriticalSection(&g_csDevIO);
        if (g_callback) g_callback(connSt, CHANGE_CONNECTED | (isWired ? CHANGE_BATTERY : 0));

        // Battery, DPI and polling come from input reports. A3 is a firmware
        // query, not an unlock; neither it nor periodic pings are necessary.

        HANDLE hReadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        OVERLAPPED ov = {0};
        ov.hEvent = hReadEvent;

        BYTE buf[65] = {0};
        DWORD bytesRead = 0;
        bool gotFirstPacket = false; // Initial reports establish DPI without showing a change notification.
        ULONGLONG openedAt=GetTickCount64();

        while (WaitForSingleObject(g_hStopEvent, 0) == WAIT_TIMEOUT) {
            // A slot switch inside the cooldown schedules one deferred read.
            // With no pending request this performs no HID transaction.
            ScheduleSleepRefresh(false);
            ResetEvent(hReadEvent);
            BOOL ok = ReadFile(hStatus, buf, 19, &bytesRead, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waitHandles[4] = { g_hStopEvent, hReadEvent, g_hDevChangeEvent, g_hRefreshEvent };
                    DWORD waitMs = 3000;
                    DWORD waitRes = WaitForMultipleObjects(4, waitHandles, FALSE, waitMs);

                    if (waitRes == WAIT_OBJECT_0) {
                        CancelIoEx(hStatus, &ov);
                        GetOverlappedResult(hStatus, &ov, &bytesRead, TRUE);
                        break;
                    } else if (waitRes == WAIT_OBJECT_0 + 2) {
                        // PnP Debounce: Windows composite device sends multiple arrival messages.
                        // Drain any consecutive events within 600ms before handling reconnect.
                        Sleep(600);
                        while (WaitForSingleObject(g_hDevChangeEvent, 0) == WAIT_OBJECT_0) {
                            ResetEvent(g_hDevChangeEvent);
                            Sleep(100);
                        }
                        CancelIoEx(hStatus, &ov);
                        GetOverlappedResult(hStatus, &ov, &bytesRead, TRUE);
                        if(KeepCurrentStatus(pathStatus))continue;
                        break;
                    } else if(waitRes==WAIT_OBJECT_0+3) {
                        CancelIoEx(hStatus,&ov);GetOverlappedResult(hStatus,&ov,&bytesRead,TRUE);
                        if(!KeepCurrentStatus(pathStatus))break;
                        ScheduleSleepRefresh();
                        continue;
                    } else if (waitRes == WAIT_TIMEOUT) {
                        CancelIoEx(hStatus, &ov);
                        GetOverlappedResult(hStatus, &ov, &bytesRead, TRUE);

                        // No heartbeat commands: expire a silent wireless
                        // device using the same freshness bound as its battery.
                        EnterCriticalSection(&g_csState);
                        bool expired=!isWired && g_currentState.isConnected &&
                            !(gotFirstPacket ? Battery::Fresh(true,g_currentState.lastBatteryUpdate) :
                              GetTickCount64()-openedAt<=Battery::FreshnessMs);
                        if(expired){g_currentState.isConnected=false;g_currentState.isCharging=false;++g_profileRevision;}
                        State stale=g_currentState;
                        LeaveCriticalSection(&g_csState);
                        if(expired && g_callback)g_callback(stale,CHANGE_CONNECTED|CHANGE_BATTERY);
                        continue;
                    } else if (waitRes == WAIT_OBJECT_0 + 1) {
                        if (!GetOverlappedResult(hStatus, &ov, &bytesRead, FALSE)) {
                            break;
                        }
                    } else {
                        CancelIoEx(hStatus, &ov);
                        GetOverlappedResult(hStatus, &ov, &bytesRead, TRUE);
                        break;
                    }
                } else {
                    break;
                }
            }

            // Parse status broadcast using ClickSync standard parser
            Rapoo::DeviceStatus devStatus;
            if (Rapoo::ParseStatusReport(buf, bytesRead, devStatus, -1)) {
                // NOTE: wired USB direct connect does NOT imply charging.
                // Trust the protocol-level charging flag for all modes.

                auto before=GetCurrentState();
                DWORD mask=ApplyPassiveStatus(devStatus,isWired,!gotFirstPacket);
                auto copySt=GetCurrentState();
                bool querySleep=!gotFirstPacket || before.profileSlot!=devStatus.profileSlot;
                gotFirstPacket = true;

                if (mask != 0 && g_callback) {
                    g_callback(copySt, mask);
                }
                if(querySleep)ScheduleSleepRefresh();
            }
        }

        EnterCriticalSection(&g_csState);
        bool wasConn = g_currentState.isConnected;
        g_currentState.isConnected = false;
        ++g_profileRevision;g_sleepRefreshPending=false;
        g_currentState.isCharging = false;
        State copySt = g_currentState;
        LeaveCriticalSection(&g_csState);
        if (wasConn && g_callback) g_callback(copySt, CHANGE_CONNECTED | CHANGE_BATTERY);

        CloseHandle(hReadEvent);
        CloseHandle(hStatus);

        EnterCriticalSection(&g_csDevIO);
        if (g_hControlDev != INVALID_HANDLE_VALUE) {
            CloseHandle(g_hControlDev);
            g_hControlDev = INVALID_HANDLE_VALUE;
        }
        if (g_hFeatureDev != INVALID_HANDLE_VALUE) {
            CloseHandle(g_hFeatureDev);
            g_hFeatureDev = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&g_csDevIO);

        Sleep(300);
    }

    return 0;
}

bool Start(HWND hNotifyWnd, StateCallback callback, bool allowSettingsQueries) {
    (void)hNotifyWnd;
    if(g_started)return false;
    g_callback = callback;
    g_allowSettingsQueries=allowSettingsQueries;

    InitializeCriticalSection(&g_csDevIO);
    InitializeCriticalSection(&g_csState);
    g_started=true;g_currentState={};

    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hDevChangeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_hRefreshEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if(!g_hStopEvent || !g_hDevChangeEvent || !g_hRefreshEvent){Stop();return false;}

    g_hWorkerThread = CreateThread(NULL, 0, HidWorkerThread, NULL, 0, NULL);
    if(!g_hWorkerThread){Stop();return false;}
    return true;
}

void BeginShutdown() {
    if(!g_started)return;
    if(g_hStopEvent)SetEvent(g_hStopEvent);
    EnterCriticalSection(&g_csState);++g_profileRevision;g_sleepRefreshPending=false;LeaveCriticalSection(&g_csState);
}
void Stop() {
    if(!g_started)return;
    BeginShutdown();
    if (g_hWorkerThread) {
        CancelSynchronousIo(g_hWorkerThread);
        WaitForSingleObject(g_hWorkerThread, INFINITE);
        CloseHandle(g_hWorkerThread);
        g_hWorkerThread = NULL;
    }
    if (g_hStopEvent) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
    }
    if (g_hDevChangeEvent) {
        CloseHandle(g_hDevChangeEvent);
        g_hDevChangeEvent = NULL;
    }
    if(g_hRefreshEvent){CloseHandle(g_hRefreshEvent);g_hRefreshEvent=NULL;}
    DeleteCriticalSection(&g_csDevIO);
    DeleteCriticalSection(&g_csState);
    g_started=false;
}

void NotifyDeviceChange() {
    if (g_hDevChangeEvent) {
        SetEvent(g_hDevChangeEvent);
    }
}

bool SetPollingRate(int hz) {
    return SetPollingRateChecked(hz)==SettingResult::Confirmed;
}
SettingResult SetPollingRateChecked(int hz,ULONGLONG expectedGeneration) {
    if(hz!=125 && hz!=250 && hz!=500 && hz!=1000 && hz!=2000 && hz!=4000 && hz!=8000)return SettingResult::InvalidValue;
    BYTE code = Rapoo::PollingHzToCode(hz);
    ProfileStamp stamp;
    SettingResult result;
    if(!WriteProfileSetting(Rapoo::ADDR_POLLING_HZ,code,&stamp,&result,expectedGeneration))return result;
    return PublishSetting(stamp,hz,false)?SettingResult::Confirmed:SettingResult::Superseded;
}

bool SetSleepTimeout(int minutes) {
    return SetSleepTimeoutChecked(minutes)==SettingResult::Confirmed;
}
SettingResult SetSleepTimeoutChecked(int minutes,ULONGLONG expectedGeneration) {
    minutes = Rapoo::ClampSleepMinutes(minutes);
    BYTE code = (BYTE)minutes;
    ProfileStamp stamp;
    SettingResult result;
    if(!WriteProfileSetting(Rapoo::ADDR_SLEEP_TIMEOUT,code,&stamp,&result,expectedGeneration))return result;
    return PublishSetting(stamp,minutes,true)?SettingResult::Confirmed:SettingResult::Superseded;
}

const WCHAR* SettingResultText(SettingResult result) {
    switch(result) {
    case SettingResult::Confirmed:return L"";
    case SettingResult::InvalidValue:return L"设置值无效，未发送";
    case SettingResult::Disconnected:return L"鼠标未连接，未发送设置";
    case SettingResult::InterfaceUnavailable:return L"配置接口不可用，未发送设置";
    case SettingResult::RetryLater:return L"接口暂不可用，请等 2 秒后重试";
    case SettingResult::RecoveryFailed:return L"接口仍忙或无应答，未发送设置";
    case SettingResult::ProfileReadFailed:return L"配置槽读取失败，未发送设置";
    case SettingResult::ProfileChanged:return L"配置槽已切换，请重新设置";
    case SettingResult::WriteFailed:return L"写入传输失败，结果未确认";
    case SettingResult::WriteAckFailed:return L"写入应答失败，结果未确认";
    case SettingResult::ReadbackFailed:return L"写入已应答，但回读未确认";
    case SettingResult::ReadbackMismatch:return L"回读值与目标不同，未确认";
    case SettingResult::Superseded:return L"结果已过期，请查看当前状态";
    case SettingResult::CurrentReadFailed:return L"当前设置读取失败，未发送设置";
    }
    return L"设置结果未知";
}

bool RefreshPollingRate() {
    EnterCriticalSection(&g_csState);
    bool passive=g_passivePolling;
    LeaveCriticalSection(&g_csState);
    if(passive){RefreshSleepTimeout();return true;}
    if(!g_allowSettingsQueries)return false;
    int curHz = 1000;
    ProfileStamp stamp;
    if (PingDevice(curHz,&stamp)) {
        if(!PublishSetting(stamp,curHz,false))return false;
        RefreshSleepTimeout();
        return true;
    }
    PublishSetting(stamp,0,false);PublishSetting(stamp,0,true);
    return false;
}

void ForceRefresh() {
    // Wake the reader to enumerate again, including USB arriving beside a receiver.
    if(g_hRefreshEvent)SetEvent(g_hRefreshEvent);
}

State GetCurrentState() {
    EnterCriticalSection(&g_csState);
    State st = g_currentState;
    st.connectionGeneration=g_profileRevision;
    st.batteryValid = st.batteryValid && st.isConnected && Battery::Fresh(true,st.lastBatteryUpdate);
    LeaveCriticalSection(&g_csState);
    return st;
}

} // namespace Device
