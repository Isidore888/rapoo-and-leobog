#include "keyboard_manager.h"
#include "keyboard_protocol.h"
#include "hid_transport.h"
#include "keyboard_notifications.h"
#include "diagnostics.h"
#include <mutex>
#include <cstdio>
namespace Keyboard {
static std::mutex mutex;
static std::mutex ioMutex; // serialize worker refresh and manual configuration
static State state;
static HANDLE worker = nullptr, stopEvent = nullptr, refreshEvent = nullptr;
static HWND notifyWindow = nullptr;
static UINT updateMessage = 0;
static bool manualRefreshPending=false,deviceChangePending=false;
static ULONGLONG deviceChangeFirst=0,deviceChangeDue=0;
static KeyboardNotifications::RadioInput radioInput;
static std::wstring verifiedRadioPath;
// All fields below and all RadioInput operations except Event() are protected
// by ioMutex. The event survives transaction pauses and closes after shutdown.
static bool radioContextEnabled=false,radioInputFaulted=false;
static ULONGLONG radioContextGeneration=0;
struct PauseRadio {
    PauseRadio(){radioInput.Pause();}
    ~PauseRadio() {
        bool resume=false;
        {std::lock_guard<std::mutex> lock(mutex);
            resume=radioContextEnabled && state.isConnected && !state.isWired &&
                state.connectionGeneration==radioContextGeneration;}
        if(resume && !radioInput.Resume()){radioInputFaulted=true;radioContextEnabled=false;}
    }
};
static thread_local LightingResult readIssue=LightingResult::ReadFailed;
static thread_local LightingResult writeIssue=LightingResult::WriteFailed;
static bool PauseBetweenUsbCommands(DWORD ms=15) {
    if(stopEvent)return WaitForSingleObject(stopEvent,ms)==WAIT_TIMEOUT;
    Sleep(ms);
    return true;
}
static bool WiredExchange(HANDLE h, BYTE cmd, BYTE param, BYTE len, BYTE* reply);
static bool ReadWiredLighting(HANDLE h,BYTE* page) {
    readIssue=LightingResult::ReadFailed;
    BYTE reply[520]{};
    if(!WiredExchange(h,0x84,0,128,reply) || !KeyboardProtocol::WiredHeader(reply,520,0x84,0,128))return false;
    if(!KeyboardProtocol::ConfigPage(reply+8,128)){Diagnostics::Log("config_page_rejected",0,0x84);readIssue=LightingResult::UnsupportedConfig;return false;}
    std::copy(reply+8,reply+136,page);return true;
}
static bool WiredExchange(HANDLE h, BYTE cmd, BYTE param, BYTE len, BYTE* reply) {
    Diagnostics::UsbQuery trace(cmd,param,len);
    if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0){trace.result=6;return false;}
    const auto deadline=GetTickCount64()+150;
    KeyboardProtocol::WiredRequest(cmd, param, len, reply);
    if (!Hid::Feature(h,reply,520,true,150,stopEvent)){trace.result=4;return false;}
    // A feature response may still be the previous command. Poll reads only;
    // send the query once, and never accept a mismatched header.
    // Guard against launching a 520-byte EP0 GET_FEATURE with <15ms remaining,
    // which would time out mid-transfer and trigger CancelIoEx on EP0.
    constexpr ULONGLONG MinGetBudgetMs=15;
    do {
        auto now=GetTickCount64();if(now>=deadline || deadline-now<=MinGetBudgetMs){trace.result=2;return false;}
        DWORD delay=DWORD(std::min<ULONGLONG>(25,deadline-now-MinGetBudgetMs));
        if(delay<5)delay=DWORD(std::min<ULONGLONG>(25,deadline-now));
        if(stopEvent) {if(WaitForSingleObject(stopEvent,delay)!=WAIT_TIMEOUT){trace.result=6;return false;}}
        else Sleep(delay);
        now=GetTickCount64();if(now>=deadline || deadline-now<MinGetBudgetMs){trace.result=2;return false;}
        std::fill(reply,reply+520,0);reply[0]=6;
        DWORD received=0;
        if(Hid::Feature(h,reply,520,false,DWORD(deadline-now),stopEvent,&received)) {
            if(GetTickCount64()>=deadline){trace.result=2;return false;}
            if(received>=DWORD(8+len) && KeyboardProtocol::WiredHeader(reply,received,cmd,param,len)){trace.result=1;return true;}
            Diagnostics::Log("reply_header_rejected",trace.id,cmd,param,0,0,received);
            readIssue=LightingResult::HeaderMismatch;
            trace.result=3;
        } else {trace.result=5;readIssue=LightingResult::ReadFailed;return false;}
    }while(GetTickCount64()<deadline);
    if(!trace.result)trace.result=2;
    return false;
}
static bool WirelessExchange(HANDLE h, BYTE cmd, BYTE* reply, bool identity) {
    BYTE request[20]; KeyboardProtocol::WirelessRequest(cmd, request);
    if (!Hid::Transfer(h, request, 20, true, 400)) return false;
    const auto deadline = GetTickCount64() + 800;
    do {
        DWORD n = 0;
        const auto now=GetTickCount64();
        DWORD remaining = DWORD(deadline > now ? deadline - now : 0);
        if (!remaining || !Hid::Transfer(h, reply, 20, false, remaining, &n)) return false;
        KeyboardProtocol::Power power;
        if (identity ? KeyboardProtocol::WirelessIdentity(reply, n) : KeyboardProtocol::WirelessPower(reply, n, power)) return true;
    } while (GetTickCount64() < deadline);
    return false;
}
static bool ReadWirelessLighting(HANDLE h,BYTE* page) {
    readIssue=LightingResult::ReadFailed;
    if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return false;
    BYTE request[20],reply[20];KeyboardProtocol::WirelessRequest(0x44,request);
    if(!Hid::Transfer(h,request,20,true,400))return false;
    bool seen[10]{};int count=0;const auto deadline=GetTickCount64()+1500;
    while(count<10) {
        if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return false;
        auto now=GetTickCount64();if(now>=deadline)return false;
        DWORD n=0;if(!Hid::Transfer(h,reply,20,false,DWORD(deadline-now),&n))return false;
        if(!KeyboardProtocol::LightingFrame(reply,n))continue;
        int index=reply[3];
        if(seen[index]) {if(!std::equal(reply+5,reply+5+reply[4],page+index*14))return false;}
        else {seen[index]=true;++count;std::copy(reply+5,reply+5+reply[4],page+index*14);}
    }
    if(!KeyboardProtocol::ConfigPage(page,128)){readIssue=LightingResult::UnsupportedConfig;return false;}
    return true;
}
static bool WriteWirelessLighting(HANDLE h,const BYTE* page) {
    writeIssue=LightingResult::WriteFailed;
    for(int index=0;index<10;++index) {
        if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return false;
        BYTE request[20],reply[20];KeyboardProtocol::LightingWritePage(page,index,request);
        if(!Hid::Transfer(h,request,20,true,400))return false;
        writeIssue=LightingResult::WriteAckFailed;
        bool ack=false;auto deadline=GetTickCount64()+500;
        while(!ack) {
            auto now=GetTickCount64();if(now>=deadline)return false;
            DWORD n=0;if(!Hid::Transfer(h,reply,20,false,DWORD(deadline-now),&n))return false;
#ifdef KEYBOARD_DIAGNOSTIC
            printf("write page=%d reply:",index);for(DWORD i=0;i<n;++i)printf(" %02X",reply[i]);puts("");
#endif
            ack=KeyboardProtocol::LightingWriteAck(reply,n,request);
        }
    }
    Sleep(100);return true;
}
static State QueryUnlocked(bool readLighting=true) {
    Hid::ScopedVidFilter vidFilter(0x258A);
    auto endpoints = Hid::Enumerate();
    State result;
    // USB wins only when this exact keyboard's identity and power reply validate.
    for (int mode = 0; mode < 2; ++mode) for (const auto& ep : endpoints) {
        if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return result;
        const auto& c = ep.caps;
        bool wired = mode == 0;
        bool match = wired ? (ep.vid == 0x258A && ep.pid == 0x010C && c.UsagePage == 0xFF00 && c.Usage == 1 && c.FeatureReportByteLength == 520)
                           : (ep.vid == 0x3554 && ep.pid == 0xFA09 && c.UsagePage == 0xFF02 && c.Usage == 2 && c.InputReportByteLength == 20 && c.OutputReportByteLength == 20);
        if (!match) continue;
        HANDLE h = Hid::Open(ep, true); if (h == INVALID_HANDLE_VALUE) continue;
        BYTE reply[520]{}; KeyboardProtocol::Power power;
        bool identity = wired ? (WiredExchange(h,0x82,1,6,reply) && KeyboardProtocol::WiredIdentity(reply,520)) : WirelessExchange(h,5,reply,true);
        if(wired && !identity)Diagnostics::Log("identity_rejected",0,0x82,1);
        bool ok = false;
        if (identity) {
            if(!wired)verifiedRadioPath=ep.path;
            result=State{};
            result.isConnected = true; result.isWired = wired;
            result.lightingWritable=true;
            if(wired && !PauseBetweenUsbCommands(10)){CloseHandle(h);Diagnostics::Log("usb_handle_closed");return result;}
            ok = wired ? (WiredExchange(h,0x87,2,2,reply) && KeyboardProtocol::WiredPower(reply,520,power)) : (WirelessExchange(h,0x0B,reply,false) && KeyboardProtocol::WirelessPower(reply,20,power));
            if(wired && !ok)Diagnostics::Log("power_rejected",0,0x87,2);
            if(readLighting) {
                if(wired && !PauseBetweenUsbCommands(10)){CloseHandle(h);Diagnostics::Log("usb_handle_closed");return result;}
                BYTE page[128];KeyboardProtocol::Lighting light;
                if(wired?ReadWiredLighting(h,page):ReadWirelessLighting(h,page)) {
                    if(KeyboardProtocol::ParseLighting(page,128,light)) {
                        result.lightingValid=true;result.brightness=light.brightness;result.lightSpeed=light.speed;result.lightEffect=light.effect;
                        result.lastLightingUpdate=GetTickCount64();
                    }
                    result.sleepValid=KeyboardProtocol::ParseSleep(page,128,result.sleepSeconds);
                    if(result.sleepValid)result.lastSleepUpdate=GetTickCount64();
                    result.configResult=LightingResult::Confirmed;
                } else {result.configResult=readIssue;result.lightingWritable=false;}
            }
        }
        CloseHandle(h);
        if(wired)Diagnostics::Log("usb_handle_closed");
        if (ok) {
            result.battery = power.percent; result.isCharging = power.charging; result.chargingKnown = power.chargingKnown;
            result.batteryValid = true; result.lastBatteryUpdate = GetTickCount64(); return result;
        }
    }
    return result;
}
State Query() {std::lock_guard<std::mutex> lock(ioMutex);PauseRadio pause;return QueryUnlocked();}

// Configuration queries only: preserve the battery report and its timestamp.
// Verify identity on each opened endpoint; never send configuration writes.
static State QueryLightingUnlocked(const State& previous) {
    State next=previous;next.lightingValid=next.sleepValid=false;next.lightingStale=next.sleepStale=false;
    next.configResult=LightingResult::Unavailable;next.lightingWritable=false;
    if(!previous.isConnected)return next;
    const bool wired=previous.isWired;
    Hid::ScopedVidFilter vidFilter(0x258A);
    auto endpoints=Hid::Enumerate();
    for(const auto& ep:endpoints) {
        if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return next;
        const auto& c=ep.caps;
        bool match=wired ? ep.vid==0x258A && ep.pid==0x010C && c.UsagePage==0xFF00 && c.Usage==1 && c.FeatureReportByteLength==520
            : ep.vid==0x3554 && ep.pid==0xFA09 && c.UsagePage==0xFF02 && c.Usage==2 && c.InputReportByteLength==20 && c.OutputReportByteLength==20;
        if(!match)continue;
        HANDLE h=Hid::Open(ep,true);if(h==INVALID_HANDLE_VALUE)continue;
        BYTE reply[520]{},page[128]{};
        bool identity=wired ? WiredExchange(h,0x82,1,6,reply) && KeyboardProtocol::WiredIdentity(reply,520) : WirelessExchange(h,5,reply,true);
        if(identity && !wired)verifiedRadioPath=ep.path;
        bool read=identity && (!wired || PauseBetweenUsbCommands(10)) && (wired?ReadWiredLighting(h,page):ReadWirelessLighting(h,page));
        CloseHandle(h);
        if(wired)Diagnostics::Log("usb_handle_closed");
        if(!read){next.configResult=identity?readIssue:LightingResult::Unavailable;continue;}
        KeyboardProtocol::Lighting light;
        if(KeyboardProtocol::ParseLighting(page,128,light)) {
            next.lightingValid=true;next.brightness=light.brightness;next.lightSpeed=light.speed;next.lightEffect=light.effect;
            next.lastLightingUpdate=GetTickCount64();
        }
        next.sleepValid=KeyboardProtocol::ParseSleep(page,128,next.sleepSeconds);
        if(next.sleepValid)next.lastSleepUpdate=GetTickCount64();
        next.lightingWritable=true;next.configResult=LightingResult::Confirmed;
        return next;
    }
    return next;
}

static bool SameContext(const State& expected) {
    std::lock_guard<std::mutex> lock(mutex);
    return state.isConnected && expected.isConnected && state.isWired==expected.isWired &&
        state.connectionGeneration==expected.connectionGeneration && state.lightEffect==expected.lightEffect;
}
static LightingResult SetSettingLocked(KeyboardProtocol::Setting setting,int value,const State* expected=nullptr) {
    bool sleep=setting==KeyboardProtocol::Setting::Sleep;
    if(sleep?(value<30 || value>7200 || value%30):(value<0 || value>4))return LightingResult::InvalidValue;
    if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return LightingResult::Superseded;
    if(expected && !SameContext(*expected))return LightingResult::Superseded;
    Hid::ScopedVidFilter vidFilter(0x258A);
    auto endpoints=Hid::Enumerate();
    for(int mode=0;mode<2;++mode)for(const auto& ep:endpoints) {
        bool wired=mode==0;
        if(expected && wired!=expected->isWired)continue;
        bool match=wired ? ep.vid==0x258A && ep.pid==0x010C && ep.caps.UsagePage==0xFF00 && ep.caps.Usage==1 && ep.caps.FeatureReportByteLength==520
            : ep.vid==0x3554 && ep.pid==0xFA09 && ep.caps.UsagePage==0xFF02 && ep.caps.Usage==2 && ep.caps.InputReportByteLength==20 && ep.caps.OutputReportByteLength==20;
        if(!match)continue;
        HANDLE h=Hid::Open(ep,true);if(h==INVALID_HANDLE_VALUE)continue;
        BYTE reply[520]{},page[128]{},confirmed[128]{};
        bool identity=wired ? WiredExchange(h,0x82,1,6,reply) && KeyboardProtocol::WiredIdentity(reply,520) : WirelessExchange(h,5,reply,true);
        if(identity && !wired)verifiedRadioPath=ep.path;
        if(!identity){CloseHandle(h);if(wired)Diagnostics::Log("usb_handle_closed");continue;}
        LightingResult result=LightingResult::ReadFailed;
        bool attempted=false;
        if((!wired || PauseBetweenUsbCommands(10)) && (wired?ReadWiredLighting(h,page):ReadWirelessLighting(h,page))) {
            BYTE original[128];std::copy(page,page+128,original);
            KeyboardProtocol::Lighting light;int seconds=0;
            bool contextOk=!expected || (SameContext(*expected) && KeyboardProtocol::ParseLighting(page,128,light) &&
                KeyboardProtocol::ParseSleep(page,128,seconds) && light.effect==expected->lightEffect &&
                light.brightness==expected->brightness && light.speed==expected->lightSpeed && seconds==expected->sleepSeconds);
            if(!contextOk)result=LightingResult::Superseded;
            else if(!(sleep?KeyboardProtocol::PatchSleep(page,128,value):KeyboardProtocol::PatchLighting(page,128,setting==KeyboardProtocol::Setting::Speed,value)))result=LightingResult::UnsupportedConfig;
            else if(std::equal(page,page+128,original)) {
                std::copy(original,original+128,confirmed);result=LightingResult::Confirmed; // Fresh full read, no setting write.
            } else if(stopEvent && WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)result=LightingResult::Superseded;
            else {
                if(wired && !PauseBetweenUsbCommands(15))result=LightingResult::Superseded;
                else {
                    attempted=true;KeyboardProtocol::WiredRequest(4,0,128,reply);std::copy(page,page+128,reply+8);
                    if(!(wired?Hid::Feature(h,reply,520,true,150,stopEvent):WriteWirelessLighting(h,page)))result=wired?LightingResult::WriteFailed:writeIssue;
                    else {
                        Sleep(25);
                        if(!(wired?ReadWiredLighting(h,confirmed):ReadWirelessLighting(h,confirmed)))result=LightingResult::ReadbackFailed;
                        else result=std::equal(page,page+128,confirmed)?LightingResult::Confirmed:LightingResult::ReadbackMismatch;
                    }
                }
            }
        } else result=readIssue;
        if(expected && !SameContext(*expected))result=LightingResult::Superseded;
        CloseHandle(h);
        if(wired)Diagnostics::Log("usb_handle_closed");
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(expected && (!state.isConnected || state.connectionGeneration!=expected->connectionGeneration ||
                state.isWired!=expected->isWired || state.lightEffect!=expected->lightEffect))result=LightingResult::Superseded;
            if(result==LightingResult::Confirmed) {
                KeyboardProtocol::Lighting light;state.lightingValid=KeyboardProtocol::ParseLighting(confirmed,128,light);
                state.lightingWritable=true;state.brightness=light.brightness;state.lightSpeed=light.speed;state.lightEffect=light.effect;
                state.lastLightingUpdate=GetTickCount64();
                state.lightingStale=state.sleepStale=false;state.configResult=LightingResult::Confirmed;
                ++state.lightingRevision;
                state.sleepValid=KeyboardProtocol::ParseSleep(confirmed,128,state.sleepSeconds);
                state.lastSleepUpdate=GetTickCount64();
            } else if(attempted && result!=LightingResult::Superseded) {
                state.lightingValid=state.sleepValid=false;state.lightingWritable=false;state.configResult=result;
            } else if(result!=LightingResult::Superseded) {state.lightingStale=state.sleepStale=true;state.lightingWritable=false;state.configResult=result;}
        }
        return result;
    }
    return LightingResult::Unavailable;
}
static LightingResult SetSetting(KeyboardProtocol::Setting setting,int value,const State* expected=nullptr) {
    LightingResult result;
    {std::lock_guard<std::mutex> lock(ioMutex);PauseRadio pause;result=SetSettingLocked(setting,value,expected);}
    if(notifyWindow)PostMessageW(notifyWindow,updateMessage,0,0);
    return result;
}
LightingResult SetLighting(bool speed,int level) {return SetSetting(speed?KeyboardProtocol::Setting::Speed:KeyboardProtocol::Setting::Brightness,level);}
LightingResult SetSleepTimeout(int seconds) {return SetSetting(KeyboardProtocol::Setting::Sleep,seconds);}
LightingResult SetLightingChecked(bool speed,int level,const State& expected) {return SetSetting(speed?KeyboardProtocol::Setting::Speed:KeyboardProtocol::Setting::Brightness,level,&expected);}
LightingResult SetSleepTimeoutChecked(int seconds,const State& expected) {return SetSetting(KeyboardProtocol::Setting::Sleep,seconds,&expected);}
const wchar_t* LightingResultText(LightingResult result) {
    switch(result) {
    case LightingResult::Confirmed:return L"";
    case LightingResult::InvalidValue:return L"请选择 0–4 档";
    case LightingResult::Unavailable:return L"键盘配置接口不可用";
    case LightingResult::ReadFailed:return L"键盘配置读取失败，未发送设置";
    case LightingResult::WriteFailed:return L"键盘设置写入失败，结果未确认";
    case LightingResult::ReadbackFailed:return L"键盘设置回读未确认，请刷新查看";
    case LightingResult::HeaderMismatch:return L"配置应答不匹配，未发送设置";
    case LightingResult::UnsupportedConfig:return L"配置格式未支持，未发送设置";
    case LightingResult::ReadbackMismatch:return L"回读与完整目标配置不同，未确认";
    case LightingResult::WriteAckFailed:return L"设置应答未确认，请刷新查看";
    case LightingResult::Superseded:return L"连接或配置已变化，请重新选择";
    }
    return L"灯光结果未知";
}
void FormatSettingPopup(KeyboardProtocol::Setting setting,int value,LightingResult result,wchar_t* title,size_t titleSize,wchar_t* detail,size_t detailSize) {
    const wchar_t* label=setting==KeyboardProtocol::Setting::Brightness?L"键盘灯光亮度":setting==KeyboardProtocol::Setting::Speed?L"键盘灯效速度":L"键盘休眠时间";
    if(result==LightingResult::Confirmed) {
        if(setting==KeyboardProtocol::Setting::Brightness)swprintf_s(title,titleSize,L"%s：%d%%",label,value*25);
        else if(setting==KeyboardProtocol::Setting::Speed)swprintf_s(title,titleSize,L"%s：%d%%",label,value*25);
        else if(value%60==0)swprintf_s(title,titleSize,L"%s：%d 分钟",label,value/60);
        else swprintf_s(title,titleSize,L"%s：%d 秒",label,value);
        detail[0]=0;
    } else {
        swprintf_s(title,titleSize,L"%s设置未完成",label);
        swprintf_s(detail,detailSize,L"%s",result==LightingResult::InvalidValue?L"设置值不在支持范围内":LightingResultText(result));
    }
}
static State MergeReading(State next,const State& previous,ULONGLONG now=GetTickCount64()) {
    // Keep only a still-fresh report from the same verified connection mode.
    // Never advance its timestamp or reuse it across a disconnect/mode change.
    if(next.isConnected && previous.isConnected && next.isWired==previous.isWired && !next.batteryValid && previous.lastBatteryUpdate) {
        next.batteryValid=Battery::Fresh(previous.batteryValid,previous.lastBatteryUpdate,now);next.battery=previous.battery;next.isCharging=previous.isCharging;next.chargingKnown=previous.chargingKnown;
        next.lastBatteryUpdate=previous.lastBatteryUpdate;
    }
    if(next.isConnected && previous.isConnected && next.isWired==previous.isWired && next.connectionGeneration==previous.connectionGeneration) {
        if(!next.lightingValid && Battery::Fresh(previous.lightingValid,previous.lastLightingUpdate,now)) {
            next.lightingValid=true;next.lightingStale=true;next.lightingWritable=false;
            next.brightness=previous.brightness;next.lightSpeed=previous.lightSpeed;next.lightEffect=previous.lightEffect;
            next.lastLightingUpdate=previous.lastLightingUpdate;
        }
        if(!next.sleepValid && Battery::Fresh(previous.sleepValid,previous.lastSleepUpdate,now)) {
            next.sleepValid=true;next.sleepStale=true;next.lightingWritable=false;
            next.sleepSeconds=previous.sleepSeconds;next.lastSleepUpdate=previous.lastSleepUpdate;
        }
    }
    return next;
}
static DWORD RefreshDelay(bool powerRead,unsigned failures) {
    if(powerRead)return 60000;
    return failures<=1?5000:failures==2?15000:failures==3?60000:300000;
}
static DWORD LightingRefreshDelay(bool read,unsigned failures) {
    if(read)return 3000;
    return failures<=1?15000:failures==2?60000:300000;
}
static void RetainConfiguration(State& next,const State& previous) {
    if(!next.isConnected || !previous.isConnected || next.isWired!=previous.isWired)return;
    next.lightingValid=previous.lightingValid;next.lightingWritable=previous.lightingWritable;
    next.brightness=previous.brightness;next.lightSpeed=previous.lightSpeed;next.lightEffect=previous.lightEffect;
    next.lastLightingUpdate=previous.lastLightingUpdate;
    next.sleepValid=previous.sleepValid;next.sleepSeconds=previous.sleepSeconds;next.lastSleepUpdate=previous.lastSleepUpdate;
    next.lightingStale=previous.lightingStale;next.sleepStale=previous.sleepStale;next.configResult=previous.configResult;
}
static ULONGLONG (WINAPI *WorkerClock)()=GetTickCount64;
static DWORD WINAPI Worker(void*) {
    unsigned failures=0,lightingFailures=0;
    ULONGLONG powerDue=0,lightingDue=0;
    KeyboardNotifications::Input hintInput;
    KeyboardNotifications::Policy hints;
    KeyboardNotifications::Policy radioHints;
    ULONGLONG hintGeneration=~ULONGLONG(0);
    ULONGLONG radioGeneration=~ULONGLONG(0);
    while (WaitForSingleObject(stopEvent,0) == WAIT_TIMEOUT) {
        auto started=WorkerClock();
        bool settling=false;ULONGLONG settleDue=0;
        {std::lock_guard<std::mutex> lock(mutex);
            settling=deviceChangePending;settleDue=deviceChangeDue;
            if(settling && started>=settleDue) {
                deviceChangePending=false;settling=false;
                if(!failures)powerDue=started;
                if(!lightingFailures)lightingDue=started;
            }
        }
        if(!settling && started>=radioHints.NextDue(hints.NextDue(std::min(powerDue,lightingDue)))) {
            bool hinted=hints.Ready(started);
            bool radioHinted=radioHints.Ready(started);
#ifdef KEYBOARD_DIAGNOSTIC
            printf("read_begin tick=%llu hinted=%d\n",started,hinted);
#endif
            bool full=started>=powerDue || radioHinted,readLighting=started>=lightingDue || hinted || radioHinted;
            State next,previous;
            bool powerRead=false,invalidated=false;
            Diagnostics::Log("refresh_begin",0,0,0,full,0,(readLighting?1:0)|(hinted?2:0)|(radioHinted?4:0));
            DWORD changes=0;ULONGLONG revision=0;
            {std::lock_guard<std::mutex> ioLock(ioMutex);
                PauseRadio pause;
                {std::lock_guard<std::mutex> lock(mutex);previous=state;}
                next=full?QueryUnlocked(readLighting):QueryLightingUnlocked(previous);powerRead=next.batteryValid;
                if(full && !readLighting)RetainConfiguration(next,previous);
                std::lock_guard<std::mutex> lock(mutex);
                invalidated=state.connectionGeneration!=previous.connectionGeneration;
                if(!invalidated) {
                    changes=LightingChanges(state,next);
                    next.connectionGeneration=state.connectionGeneration+
                        (state.isConnected!=next.isConnected || (next.isConnected && state.isWired!=next.isWired)?1:0);
                    next.lightingRevision=state.lightingRevision+(readLighting && next.lightingValid?1:0);
                    state=MergeReading(next,state);revision=state.lightingRevision;
                }
            }
            auto now=WorkerClock();
            Diagnostics::Log("refresh_end",0,0,0,next.isConnected,0,(next.isWired?1:0)|(next.batteryValid?2:0)|(next.lightingValid?4:0)|(next.sleepValid?8:0)|(invalidated?16:0));
            if(invalidated) {
                // Discard stale data, then wait rather than immediately resend.
                powerDue=std::max(powerDue,now+1000);lightingDue=std::max(lightingDue,now+1000);
                hints.Disable();radioHints.Disable();
                continue;
            }
            if(full) {failures=powerRead?0:std::min(failures+1,4u);powerDue=now+RefreshDelay(powerRead,failures);}
            if(!next.isConnected)lightingDue=powerDue;
            else if(readLighting) {
                lightingFailures=next.lightingValid?0:std::min(lightingFailures+1,3u);
                lightingDue=now+LightingRefreshDelay(next.lightingValid,lightingFailures);
            } else if(!previous.isConnected || previous.isWired!=next.isWired)lightingDue=now;
            auto published=GetCurrentState();
            {
                std::lock_guard<std::mutex> ioLock(ioMutex);
                published=GetCurrentState();
                if(radioGeneration!=published.connectionGeneration) {
                    radioInput.Pause();radioHints.Reset(now);radioGeneration=published.connectionGeneration;
                    radioContextGeneration=radioGeneration;radioInputFaulted=false;
                    radioContextEnabled=published.isConnected && !published.isWired && published.lightingValid &&
                        !verifiedRadioPath.empty();
                    if(!radioContextEnabled || !radioInput.Configure(verifiedRadioPath)) {
                        radioContextEnabled=false;radioHints.Disable();
                    }
                    radioHinted=false;
                }
                if(radioInputFaulted)radioHints.Disable();
                if(hintGeneration!=published.connectionGeneration) {
                    hintInput.Close();hints.Reset(now);hintGeneration=published.connectionGeneration;
                    if(!published.isConnected || !published.isWired || !published.lightingValid || !hintInput.Attach())hints.Disable();
                    hinted=false;
                }
            }
            if(readLighting)hints.Read(next.lightingValid && next.sleepValid,
                next.brightness!=previous.brightness || next.lightSpeed!=previous.lightSpeed || next.lightEffect!=previous.lightEffect,hinted,now);
            if(readLighting)radioHints.Read(next.lightingValid && next.sleepValid && (!radioHinted || next.batteryValid),
                next.brightness!=previous.brightness || next.lightSpeed!=previous.lightSpeed || next.lightEffect!=previous.lightEffect ||
                    next.sleepSeconds!=previous.sleepSeconds || next.battery!=previous.battery,radioHinted,now);
            if(!radioHints.enabled || !hints.enabled) {
                std::lock_guard<std::mutex> ioLock(ioMutex);
                if(!radioHints.enabled){radioContextEnabled=false;radioInput.Pause();}
                if(!hints.enabled)hintInput.Close();
            }
            PostMessageW(notifyWindow,updateMessage,0,0);
            if(changes)PostMessageW(notifyWindow,SettingsChangedMessage,changes,static_cast<LPARAM>(revision));
        }
        HANDLE events[4] = {stopEvent,refreshEvent};DWORD countEvents=2,usbIndex=MAXDWORD,radioIndex=MAXDWORD;
        if(hintInput.Event()){usbIndex=countEvents;events[countEvents++]=hintInput.Event();}
        if(radioInput.Event()){radioIndex=countEvents;events[countEvents++]=radioInput.Event();}
        auto due=radioHints.NextDue(hints.NextDue(std::min(powerDue,lightingDue)));auto now=WorkerClock();
        {std::lock_guard<std::mutex> lock(mutex);if(deviceChangePending)due=deviceChangeDue;}
        DWORD wait = WaitForMultipleObjects(countEvents,events,FALSE,DWORD(due>now?due-now:1));
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0+1) {
            std::lock_guard<std::mutex> lock(mutex);
            if(manualRefreshPending){manualRefreshPending=false;failures=lightingFailures=0;powerDue=lightingDue=0;}
        }
        if(usbIndex!=MAXDWORD && wait==WAIT_OBJECT_0+usbIndex) {
            std::array<BYTE,4> report{};DWORD count=0;
            std::lock_guard<std::mutex> ioLock(ioMutex);
            if(!hintInput.Consume(report,count))hints.Disable();
            else {
                const bool accepted=hints.Hint(report.data(),count,WorkerClock());
                Diagnostics::Log("usb_change_hint",0,0,0,accepted,0,count);
#ifdef KEYBOARD_DIAGNOSTIC
                printf("notification tick=%llu accepted=%d\n",WorkerClock(),accepted);
#else
                (void)accepted;
#endif
            }
            if(!hints.enabled)hintInput.Close();
        }
        if(radioIndex!=MAXDWORD && wait==WAIT_OBJECT_0+radioIndex) {
            std::array<BYTE,20> report{};DWORD count=0;
            std::lock_guard<std::mutex> ioLock(ioMutex);
            const int consumed=radioInput.Consume(report,count);
            bool context=false;
            {std::lock_guard<std::mutex> lock(mutex);context=radioContextEnabled && state.isConnected &&
                !state.isWired && state.connectionGeneration==radioContextGeneration;}
            if(consumed<0)radioHints.Disable();
            else if(consumed>0 && context) {
                const bool accepted=radioHints.RadioHint(report.data(),count,WorkerClock());
#ifdef KEYBOARD_DIAGNOSTIC
                printf("radio_notification tick=%llu accepted=%d\n",WorkerClock(),accepted);
#else
                (void)accepted;
#endif
            }
            if(!radioHints.enabled){radioContextEnabled=false;radioInput.Pause();}
        }
    }
    return 0;
}
bool Start(HWND window, UINT message) {
    notifyWindow = window; updateMessage = message;
    stopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr); refreshEvent = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!stopEvent || !refreshEvent) { Stop(); return false; }
    worker = CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
    if (!worker) { Stop(); return false; } return true;
}
void BeginShutdown() {
    if(stopEvent)SetEvent(stopEvent);
    std::lock_guard<std::mutex> lock(mutex);++state.connectionGeneration;
}
void Stop() {
    BeginShutdown();
    if (worker) { CancelSynchronousIo(worker); WaitForSingleObject(worker,INFINITE); CloseHandle(worker); worker=nullptr; }
    {std::lock_guard<std::mutex> ioLock(ioMutex);radioContextEnabled=false;radioInput.Close();verifiedRadioPath.clear();}
    if (stopEvent) {CloseHandle(stopEvent);stopEvent=nullptr;}
    if (refreshEvent) {CloseHandle(refreshEvent);refreshEvent=nullptr;}
    {std::lock_guard<std::mutex> lock(mutex);manualRefreshPending=deviceChangePending=false;}
}
void ForceRefresh() {
    Diagnostics::Log("manual_refresh");
    {std::lock_guard<std::mutex> lock(mutex);manualRefreshPending=true;}
    if(refreshEvent)SetEvent(refreshEvent);
}
void NotifyDeviceChange() {
    Diagnostics::Log("keyboard_device_change");
    auto now=WorkerClock();
    {std::lock_guard<std::mutex> lock(mutex);++state.connectionGeneration;
     state.lightingValid=state.sleepValid=false;
     if(!deviceChangePending)deviceChangeFirst=now;
     deviceChangePending=true;deviceChangeDue=std::min(now+1000,deviceChangeFirst+2000);}
    if(refreshEvent)SetEvent(refreshEvent);
}
State GetCurrentState() {
    std::lock_guard<std::mutex> lock(mutex); State copy = state;
    copy.batteryValid = Battery::Fresh(copy.batteryValid,copy.lastBatteryUpdate);
    copy.lightingValid=copy.isConnected && Battery::Fresh(copy.lightingValid,copy.lastLightingUpdate);
    copy.sleepValid=copy.isConnected && Battery::Fresh(copy.sleepValid,copy.lastSleepUpdate);
    copy.lightingWritable=copy.lightingWritable && copy.lightingValid && copy.sleepValid && !copy.lightingStale && !copy.sleepStale;
    return copy;
}
}
