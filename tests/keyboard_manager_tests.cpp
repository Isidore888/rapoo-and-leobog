#include <windows.h>
#include <cassert>
#include <cstdio>
#include <array>
#include <deque>
#include "../src/hid_transport.h"
#include "../src/keyboard_protocol.h"
#include "../src/keyboard_manager.h"
#include "../src/keyboard_notifications.h"
#include "../src/diagnostics.h"
static BYTE page[128]{},lastRequest[520]{};
static int configWrites=0;
static bool identityOk=true,failConfigRead=false,failWrite=false,mismatch=false;
static int staleHeaders=0,queryWrites=0;
static DWORD featureSetDelay=0,featureGetDelay=0,featureReplySize=520;
static int powerQueries=0,configurationQueries=0;
static int enumerateCalls=0;
static unsigned pressureScenario=0;
static bool failPower=false;
static bool radio=false,badAck=false,missingPage=false,badChecksum=false,conflictingDuplicate=false;
static std::deque<std::array<BYTE,20>> replies;
static std::vector<DWORD> observedWaits;
static std::vector<bool> observedBattery;
static bool simulateChanges=false;
static bool generationOnRead=false,generationOnWrite=false,badConfigHeader=false;
static void ChangeGeneration();
static DWORD changedMask=0;
static LPARAM changedRevision=0;
static ULONGLONG workerTime=0;
static unsigned waitsToStop=3;
static unsigned notificationScenario=0,notificationWait=0;
static int notificationBaselineQueries=0,notificationOpens=0,notificationCloses=0;
static unsigned radioScenario=0,radioWait=0;
static unsigned pnpScenario=0,pnpWait=0;
static int pnpConfig=0,pnpPower=0;
static bool radioReaderActive=false;
static int radioBaselineConfig=0,radioBaselinePower=0;
static void ManualRadioTransaction();
static void ChangeToUsb();
namespace KeyboardNotifications {
class FakeRadioInput {
    HANDLE event=nullptr;
public:
    ~FakeRadioInput(){Close();}
    void Pause(){radioReaderActive=false;if(event)ResetEvent(event);}
    bool Resume(){radioReaderActive=event!=nullptr;return radioReaderActive;}
    bool Configure(const std::wstring& path){assert(path==L"fake-radio");Pause();if(!radioScenario)return false;
        if(!event)event=CreateEventW(nullptr,TRUE,FALSE,nullptr);return Resume();}
    HANDLE Event() const{return event;}
    int Consume(std::array<BYTE,20>& packet,DWORD& size) {
        if(radioScenario==6){Pause();return -1;}
        if(!radioReaderActive)return 0;
        packet={0x13,0x0A,1,0,4,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0x29};size=20;
        if(radioScenario>=10) {
            packet[5]=2;packet[6]=radioScenario==10?0:radioScenario==12?2:1;
            packet[19]=0;for(int i=0;i<19;++i)packet[19]+=packet[i];
        }
        if(radioScenario==3)packet[19]^=1;
        return 1;
    }
    void Close(){Pause();if(event){CloseHandle(event);event=nullptr;}}
};
class FakeInput {
    HANDLE event=nullptr;
public:
    ~FakeInput(){Close();}
    bool Attach(){Close();if(!notificationScenario)return false;event=CreateEventW(nullptr,TRUE,FALSE,nullptr);++notificationOpens;return event!=nullptr;}
    void Close(){if(event){CloseHandle(event);event=nullptr;++notificationCloses;}}
    HANDLE Event() const{return event;}
    bool Consume(std::array<BYTE,4>& value,DWORD& count){
        if(notificationScenario==6){Close();return false;}
        value=notificationScenario==5?std::array<BYTE,4>{3,8,0,2}:std::array<BYTE,4>{3,8,0,1};count=4;return true;
    }
};
}
static ULONGLONG WINAPI FakeClock(){return workerTime;}
static BOOL WINAPI FakePost(HWND,UINT msg,WPARAM mask,LPARAM revision) {
    if(msg==Keyboard::SettingsChangedMessage){changedMask=DWORD(mask);changedRevision=revision;}
    else observedBattery.push_back(Keyboard::GetCurrentState().batteryValid);
    return TRUE;
}
static DWORD WINAPI FakeWait(DWORD count,const HANDLE* events,BOOL all,DWORD timeout) {
    if(pressureScenario) {
        assert(count==2 && !all && timeout==3000);observedWaits.push_back(timeout);
        if(pressureScenario==2 && observedWaits.size()==1){workerTime+=500;Keyboard::ForceRefresh();return WAIT_OBJECT_0+1;}
        workerTime+=timeout;
        if(observedWaits.size()<3)return WAIT_TIMEOUT;
        SetEvent(events[0]);return WAIT_OBJECT_0;
    }
    if(pnpScenario) {
        assert(count==2 && !all);++pnpWait;
        if(pnpWait==1){assert(timeout==(pnpScenario==1?3000u:15000u));workerTime+=500;Keyboard::NotifyDeviceChange();return WAIT_OBJECT_0+1;}
        if(pnpScenario==1) {
            if(pnpWait==2 || pnpWait==3){assert(timeout==1000 && configurationQueries==pnpConfig+1);workerTime+=900;Keyboard::NotifyDeviceChange();return WAIT_OBJECT_0+1;}
            if(pnpWait==4){assert(timeout==200 && configurationQueries==pnpConfig+1);workerTime+=timeout;return WAIT_TIMEOUT;}
            assert(timeout==3000 && configurationQueries==pnpConfig+2 && powerQueries==pnpPower+2);
        } else {
            if(pnpWait==2){assert(timeout==1000);workerTime+=timeout;return WAIT_TIMEOUT;}
            if(pnpWait==3){assert(timeout==13500 && configurationQueries==pnpConfig+1 && powerQueries==pnpPower+2);workerTime+=timeout;return WAIT_TIMEOUT;}
            assert(timeout==46500 && configurationQueries==pnpConfig+2);
        }
        SetEvent(events[0]);return WAIT_OBJECT_0;
    }
    if(radioScenario) {
        assert(!all && count==3);++radioWait;
        if(radioWait==1) {
            assert(timeout==3000 && radioReaderActive);
            assert(configurationQueries==radioBaselineConfig+1 && powerQueries==radioBaselinePower+1);
            workerTime+=500;if(radioScenario!=4 && radioScenario!=11)page[76]=3;
            if(radioScenario==10)identityOk=false;
            if(radioScenario==5 || radioScenario==13){ChangeToUsb();radio=false;}
            return WAIT_OBJECT_0+2;
        }
        if(radioWait==2) {
            assert(timeout==((radioScenario==3 || radioScenario==5 || radioScenario==6 || radioScenario==12 || radioScenario==13)?2500u:1000u));
            if(radioScenario==2)failConfigRead=true;
            if(radioScenario==8 || radioScenario==9)ManualRadioTransaction();
            workerTime+=timeout;return WAIT_TIMEOUT;
        }
        if(radioScenario==7 && radioWait==3){workerTime+=600;return WAIT_OBJECT_0+2;}
        if(radioScenario==7 && radioWait==4){assert(timeout==1000);workerTime+=timeout;return WAIT_TIMEOUT;}
        if(radioScenario==2 || radioScenario==4 || radioScenario==6 || radioScenario==7 || radioScenario==10 || radioScenario==11)assert(!radioReaderActive);
        SetEvent(events[0]);return WAIT_OBJECT_0;
    }
    if(notificationScenario) {
        assert(!all);++notificationWait;
        if(notificationScenario==3) {
            assert(count==3 && timeout==3000);
            if(notificationWait==1){page[76]=3;workerTime+=timeout;return WAIT_TIMEOUT;}
            SetEvent(events[0]);return WAIT_OBJECT_0;
        }
        if(notificationWait==1) {
            assert(count==3 && configurationQueries==notificationBaselineQueries+1 && timeout==3000);
            workerTime+=500;page[76]=3;return WAIT_OBJECT_0+2;
        }
        if(notificationWait==2) {
            if(notificationScenario>=5){assert(count==(notificationScenario==6?2u:3u) && timeout==2500);workerTime+=timeout;return WAIT_TIMEOUT;}
            assert(count==3 && configurationQueries==notificationBaselineQueries+1 && timeout==1000);
            if(notificationScenario==4){Keyboard::NotifyDeviceChange();page[76]=4;return WAIT_OBJECT_0+1;}
            workerTime+=600;return WAIT_OBJECT_0+2;
        }
        if(notificationWait==3) {
            if(notificationScenario==4){assert(configurationQueries==notificationBaselineQueries+1 && timeout==1000);workerTime+=timeout;return WAIT_TIMEOUT;}
            if(notificationScenario>=4){assert(configurationQueries==notificationBaselineQueries+2 && timeout==3000);SetEvent(events[0]);return WAIT_OBJECT_0;}
            assert(count==3 && configurationQueries==notificationBaselineQueries+1 && timeout==1000);
            if(notificationScenario==2)failConfigRead=true;
            workerTime+=timeout;return WAIT_TIMEOUT;
        }
        if(notificationScenario==2) {
            assert(count==2 && timeout==15000 && configurationQueries==notificationBaselineQueries+2);
            SetEvent(events[0]);return WAIT_OBJECT_0;
        }
        if(notificationWait==4) {
            if(notificationScenario==4){assert(configurationQueries==notificationBaselineQueries+2 && timeout==3000);SetEvent(events[0]);return WAIT_OBJECT_0;}
            assert(count==3 && configurationQueries==notificationBaselineQueries+2);
            workerTime+=600;return WAIT_OBJECT_0+2; // simulated delayed query echo
        }
        if(notificationWait==5){assert(count==3 && timeout==1000);workerTime+=timeout;return WAIT_TIMEOUT;}
        assert(notificationWait==6 && count==2 && configurationQueries==notificationBaselineQueries+3);
        SetEvent(events[0]);return WAIT_OBJECT_0;
    }
    assert(count==2 && !all);observedWaits.push_back(timeout);
    workerTime+=timeout;
    if(observedWaits.size()==1){failPower=false;if(simulateChanges){page[76]=2;page[77]=0x10;}return WAIT_TIMEOUT;}
    if(observedWaits.size()<waitsToStop)return WAIT_TIMEOUT;
    SetEvent(events[0]);return WAIT_OBJECT_0;
}
static void Checksum(std::array<BYTE,20>& r) {r[19]=0;for(int i=0;i<19;++i)r[19]+=r[i];}
namespace Hid {
std::vector<Endpoint> FakeEnumerate() {
    ++enumerateCalls;
    if(radio){Endpoint ep;ep.path=L"fake-radio";ep.vid=0x3554;ep.pid=0xFA09;ep.caps.UsagePage=0xFF02;ep.caps.Usage=2;ep.caps.InputReportByteLength=ep.caps.OutputReportByteLength=20;return {ep};}
    Endpoint ep;ep.path=L"fake-usb";ep.vid=0x258A;ep.pid=0x010C;ep.caps.UsagePage=0xFF00;ep.caps.Usage=1;ep.caps.FeatureReportByteLength=520;return {ep};
}
HANDLE FakeOpen(const Endpoint&,bool overlapped){assert(overlapped && !radioReaderActive);replies.clear();return CreateEventW(nullptr,FALSE,FALSE,nullptr);}
bool FakeTransfer(HANDLE,BYTE* data,DWORD n,bool write,DWORD,DWORD* received=nullptr) {
    assert(n==20);
    if(!write){if(replies.empty())return false;auto r=replies.front();replies.pop_front();std::copy(r.begin(),r.end(),data);if(received)*received=20;return true;}
    assert(KeyboardProtocol::WirelessFrame(data,n,data[1]));
    std::array<BYTE,20> r{};r[0]=0x13;r[2]=1;
    switch(data[1]) {
    case 5:r[1]=5;r[4]=10;std::copy(KeyboardProtocol::Signature,KeyboardProtocol::Signature+6,r.begin()+5);if(!identityOk)r[5]^=1;break;
    case 0x0B:++powerQueries;r[1]=0x0A;r[4]=4;r[5]=5;r[6]=100;r[7]=1;break;
    case 0x44:
        ++configurationQueries;
        if(failConfigRead)return false;
        for(int index=9;index>=0;--index) {
            if(missingPage && index==0)continue;
            KeyboardProtocol::LightingWritePage(page,index,r.data());r[1]=0x44;
            if(mismatch && index==5)r[11]^=1;
            Checksum(r);if(badChecksum && index==0)r[19]^=1;
            replies.push_back(r);
            if(index==9){if(conflictingDuplicate){r[5]^=1;Checksum(r);}replies.push_back(r);}
        }
        return true;
    case 4:
        ++configWrites;if(failWrite)return false;
        assert(data[2]==10 && data[3]<10 && data[4]==(data[3]==9?2:14));
        std::copy(data+5,data+5+data[4],page+data[3]*14);
        std::copy(data,data+20,r.begin());
        if(badAck){r[3]=(r[3]+1)%10;Checksum(r);}
        replies.push_back(r);return true;
    default:assert(false);
    }
    Checksum(r);replies.push_back(r);return true;
}
}
static BOOLEAN WINAPI FakeSet(HANDLE,PVOID data,ULONG n) {
    assert(n==520);std::copy((BYTE*)data,(BYTE*)data+n,lastRequest);
    if(lastRequest[1]==4) {
        ++configWrites;if(failWrite)return FALSE;
        assert(KeyboardProtocol::WiredHeader(lastRequest,n,4,0,128));
        std::copy(lastRequest+8,lastRequest+136,page);
        if(generationOnWrite){generationOnWrite=false;ChangeGeneration();}
    }else {assert(lastRequest[1]==0x82 || lastRequest[1]==0x87 || lastRequest[1]==0x84);++queryWrites;if(lastRequest[1]==0x87)++powerQueries;if(lastRequest[1]==0x84)++configurationQueries;}
    return TRUE;
}
static BOOLEAN WINAPI FakeGet(HANDLE,PVOID data,ULONG n) {
    assert(n==520);auto r=(BYTE*)data;std::copy(lastRequest,lastRequest+n,r);
    if(staleHeaders>0){--staleHeaders;r[1]^=1;return TRUE;}
    if(failPower && r[1]==0x87)return FALSE;
    switch(r[1]) {
    case 0x82:std::copy(KeyboardProtocol::Signature,KeyboardProtocol::Signature+6,r+8);if(!identityOk)r[8]^=1;break;
    case 0x87:r[8]=100;r[9]=0x10;break;
    case 0x84:if(failConfigRead)return FALSE;std::copy(page,page+128,r+8);if(mismatch)r[84]^=1;
        if(badConfigHeader)r[1]^=1;
        if(generationOnRead){generationOnRead=false;ChangeGeneration();}
        break;
    default:assert(false);
    }
    return TRUE;
}
namespace Hid {
bool FakeFeature(HANDLE h,BYTE* data,DWORD size,bool write,DWORD timeout,HANDLE stop,DWORD* received=nullptr) {
    assert(timeout>0 && timeout<=150 && !radio);
    if(stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0)return false;
    // Deliberately return a late success to check the manager's own deadline too.
    Sleep(write?featureSetDelay:featureGetDelay);
    bool ok=write?FakeSet(h,data,size):FakeGet(h,data,size);
    if(received)*received=ok?featureReplySize:0;
    return ok;
}
}
#define Enumerate FakeEnumerate
#define Open FakeOpen
#define Transfer FakeTransfer
#define Feature FakeFeature
#define WaitForMultipleObjects FakeWait
#define PostMessageW FakePost
#define Input FakeInput
#define RadioInput FakeRadioInput
#include "../src/keyboard_manager.cpp"
#undef RadioInput
#undef Input
#undef Enumerate
#undef Open
#undef Transfer
#undef Feature
#undef WaitForMultipleObjects
#undef PostMessageW
static void ChangeGeneration(){std::lock_guard<std::mutex> lock(Keyboard::mutex);++Keyboard::state.connectionGeneration;Keyboard::state.lightingValid=Keyboard::state.sleepValid=false;}
static void ChangeToUsb(){std::lock_guard<std::mutex> lock(Keyboard::mutex);++Keyboard::state.connectionGeneration;Keyboard::state.isWired=true;Keyboard::state.lightingValid=Keyboard::state.sleepValid=false;}
static void ManualRadioTransaction() {
    const auto event=Keyboard::radioInput.Event();
    if(radioScenario==8)assert(Keyboard::Query().lightingValid);
    else {auto expected=Keyboard::GetCurrentState();page[76]=4;
        assert(Keyboard::SetLightingChecked(false,4,expected)==Keyboard::LightingResult::Confirmed);}
    assert(Keyboard::radioInput.Event()==event && radioReaderActive);
}
int main() {
    page[10]=10;page[24]=20;page[56]=page[57]=0xFF;page[126]=0x5A;page[127]=0xA5;page[76]=3;page[77]=0x37;
    BYTE timedReply[520]{};int timedQueries=queryWrites;
    featureSetDelay=300;assert(!Keyboard::WiredExchange(nullptr,0x82,1,6,timedReply));featureSetDelay=0;
    featureGetDelay=300;assert(!Keyboard::WiredExchange(nullptr,0x82,1,6,timedReply));featureGetDelay=0;
    featureSetDelay=90;featureGetDelay=45;assert(!Keyboard::WiredExchange(nullptr,0x82,1,6,timedReply));featureSetDelay=featureGetDelay=0;
    featureReplySize=8;assert(!Keyboard::WiredExchange(nullptr,0x82,1,6,timedReply));featureReplySize=520;
    assert(queryWrites==timedQueries+4); // one send, including late/mismatched replies
    staleHeaders=2;int queryBefore=queryWrites;
    auto current=Keyboard::Query();assert(current.batteryValid && current.battery==100 && current.lightingValid && current.brightness==3 && current.lightSpeed==3);
    assert(staleHeaders==0 && queryWrites==queryBefore+3 && current.sleepValid && current.sleepSeconds==600);
    failPower=true;auto failedPower=Keyboard::Query();failPower=false;
    assert(failedPower.isConnected && !failedPower.batteryValid && failedPower.lightingValid);
    auto merged=Keyboard::MergeReading(failedPower,current,current.lastBatteryUpdate+1000);
    assert(merged.batteryValid && merged.lastBatteryUpdate==current.lastBatteryUpdate);
    assert(!Keyboard::MergeReading(failedPower,current,current.lastBatteryUpdate+Battery::FreshnessMs+1).batteryValid);
    failedPower.isWired=false;assert(!Keyboard::MergeReading(failedPower,current,current.lastBatteryUpdate+1000).batteryValid);
    failedPower.isConnected=false;assert(!Keyboard::MergeReading(failedPower,current,current.lastBatteryUpdate+1000).batteryValid);
    assert(Keyboard::RefreshDelay(true,0)==60000 && Keyboard::RefreshDelay(false,1)==5000 && Keyboard::RefreshDelay(false,2)==15000 && Keyboard::RefreshDelay(false,3)==60000 && Keyboard::RefreshDelay(false,4)==300000);
    assert(Keyboard::LightingRefreshDelay(true,0)==3000 && Keyboard::LightingRefreshDelay(false,1)==15000 && Keyboard::LightingRefreshDelay(false,2)==60000 && Keyboard::LightingRefreshDelay(false,3)==300000);
    // Fast reads preserve battery/timestamp, issue no power query or setting write.
    for(bool mode:{false,true}) {
        radio=mode;auto prior=current;prior.isWired=!mode;
        int oldWrites=configWrites,oldPower=powerQueries;
        auto fast=Keyboard::QueryLightingUnlocked(prior);
        assert(fast.lightingValid && fast.sleepValid && fast.battery==prior.battery && fast.lastBatteryUpdate==prior.lastBatteryUpdate);
        assert(configWrites==oldWrites && powerQueries==oldPower);
        identityOk=false;fast=Keyboard::QueryLightingUnlocked(prior);identityOk=true;
        assert(!fast.lightingValid && !fast.sleepValid && fast.lastBatteryUpdate==prior.lastBatteryUpdate);
        assert(configWrites==oldWrites && powerQueries==oldPower);
    }
    // Guarded asynchronous path: freshly read same value never writes; stale
    // UI/effect/connection contexts cannot patch a configuration page.
    radio=false;current=Keyboard::Query();
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=current;}
    int guardedWrites=configWrites;
    assert(Keyboard::SetLightingChecked(false,current.brightness,current)==Keyboard::LightingResult::Confirmed && configWrites==guardedWrites);
    auto wrong=current;++wrong.connectionGeneration;
    assert(Keyboard::SetLightingChecked(false,1,wrong)==Keyboard::LightingResult::Superseded && configWrites==guardedWrites);
    page[10]=11;
    assert(Keyboard::SetLightingChecked(false,1,current)==Keyboard::LightingResult::Superseded && configWrites==guardedWrites);page[10]=10;
    generationOnRead=true;
    assert(Keyboard::SetLightingChecked(false,1,current)==Keyboard::LightingResult::Superseded && configWrites==guardedWrites);
    current=Keyboard::Query();{std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=current;}
    generationOnWrite=true;
    assert(Keyboard::SetLightingChecked(false,2,current)==Keyboard::LightingResult::Superseded && configWrites==guardedWrites+1 && !Keyboard::GetCurrentState().lightingValid);
    page[76]=3;
    badConfigHeader=true;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::HeaderMismatch);badConfigHeader=false;
    page[56]=0;
    auto formatWrites=configWrites;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::UnsupportedConfig && configWrites==formatWrites);page[56]=0xFF;
    current=Keyboard::Query();
    auto failed=Keyboard::State{};failed.isConnected=true;failed.isWired=current.isWired;failed.connectionGeneration=current.connectionGeneration;
    failed.configResult=Keyboard::LightingResult::ReadFailed;
    auto retained=Keyboard::MergeReading(failed,current,current.lastLightingUpdate+1000);
    assert(retained.lightingValid && retained.lightingStale && retained.sleepStale && !retained.lightingWritable);
    assert(retained.lastLightingUpdate==current.lastLightingUpdate && retained.lastSleepUpdate==current.lastSleepUpdate);
    assert(!Keyboard::MergeReading(failed,current,current.lastLightingUpdate+Battery::FreshnessMs+1).lightingValid);
    ++failed.connectionGeneration;assert(!Keyboard::MergeReading(failed,current).lightingValid);
    assert(!Keyboard::LightingChanges(retained,current));
    radio=false;
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=current;}
    BYTE original[128];std::copy(page,page+128,original);
    assert(Keyboard::SetLighting(false,2)==Keyboard::LightingResult::Confirmed && page[76]==2);
    for(int i=0;i<128;++i)assert(page[i]==(i==76?2:original[i]));
    assert(Keyboard::GetCurrentState().brightness==2 && Keyboard::GetCurrentState().lightSpeed==3);
    assert(Keyboard::SetLighting(true,1)==Keyboard::LightingResult::Confirmed && page[77]==0x17);
    std::copy(page,page+128,original);
    assert(Keyboard::SetSleepTimeout(300)==Keyboard::LightingResult::Confirmed && Keyboard::GetCurrentState().sleepSeconds==300);
    for(int i=0;i<128;++i)assert(page[i]==(i==24?10:original[i]));
    assert(Keyboard::SetSleepTimeout(600)==Keyboard::LightingResult::Confirmed);
    int noWrite=configWrites;
    for(int value:{0,29,31,7201})assert(Keyboard::SetSleepTimeout(value)==Keyboard::LightingResult::InvalidValue && configWrites==noWrite);
    identityOk=false;int before=configWrites;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::Unavailable && configWrites==before);identityOk=true;
    failConfigRead=true;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::ReadFailed && configWrites==before);failConfigRead=false;
    failWrite=true;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::WriteFailed && !Keyboard::GetCurrentState().lightingValid);failWrite=false;
    assert(Keyboard::SetLighting(false,2)==Keyboard::LightingResult::Confirmed);
    mismatch=true;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::ReadbackMismatch && !Keyboard::GetCurrentState().lightingValid);mismatch=false;
    before=configWrites;assert(Keyboard::SetLighting(true,5)==Keyboard::LightingResult::InvalidValue && configWrites==before);
    assert(Keyboard::LightingResultText(Keyboard::LightingResult::Confirmed)[0]==0);
    radio=true;page[76]=3;page[77]=0x37;
    current=Keyboard::Query();assert(current.isConnected && !current.isWired && current.batteryValid && current.lightingValid && current.brightness==3 && current.lightSpeed==3);
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=current;}
    std::copy(page,page+128,original);before=configWrites;
    assert(Keyboard::SetLighting(false,2)==Keyboard::LightingResult::Confirmed && configWrites==before+10);
    for(int i=0;i<128;++i)assert(page[i]==(i==76?2:original[i]));
    assert(Keyboard::SetLighting(true,1)==Keyboard::LightingResult::Confirmed && page[77]==0x17);
    std::copy(page,page+128,original);before=configWrites;
    assert(Keyboard::SetSleepTimeout(300)==Keyboard::LightingResult::Confirmed && configWrites==before+10 && page[24]==10);
    for(int i=0;i<128;++i)assert(page[i]==(i==24?10:original[i]));
    assert(Keyboard::SetSleepTimeout(600)==Keyboard::LightingResult::Confirmed);
    for(bool* invalid:{&missingPage,&badChecksum,&conflictingDuplicate}) {
        *invalid=true;assert(!Keyboard::Query().lightingValid);before=configWrites;
        assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::ReadFailed && configWrites==before);*invalid=false;
    }
    badAck=true;before=configWrites;
    assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::WriteAckFailed && configWrites==before+1 && !Keyboard::GetCurrentState().lightingValid);badAck=false;
    assert(Keyboard::SetLighting(false,2)==Keyboard::LightingResult::Confirmed);
    mismatch=true;assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::ReadbackMismatch);mismatch=false;
    identityOk=false;before=configWrites;assert(Keyboard::SetLighting(false,1)==Keyboard::LightingResult::Unavailable && configWrites==before);
    wchar_t title[96],detail[96];
    Keyboard::FormatSettingPopup(KeyboardProtocol::Setting::Brightness,3,Keyboard::LightingResult::Confirmed,title,96,detail,96);
    assert(wcsstr(title,L"75%") && !wcsstr(title,L"已确认") && !detail[0]);
    for(int level=0;level<=4;++level) {
        Keyboard::FormatSettingPopup(KeyboardProtocol::Setting::Speed,level,Keyboard::LightingResult::Confirmed,title,96,detail,96);
        wchar_t percent[16];swprintf_s(percent,L"%d%%",level*25);assert(wcsstr(title,percent) && !detail[0]);
    }
    Keyboard::FormatSettingPopup(KeyboardProtocol::Setting::Sleep,600,Keyboard::LightingResult::Confirmed,title,96,detail,96);assert(wcsstr(title,L"10 分钟"));
    Keyboard::FormatSettingPopup(KeyboardProtocol::Setting::Speed,4,Keyboard::LightingResult::ReadbackFailed,title,96,detail,96);assert(wcsstr(title,L"未完成") && !wcsstr(title,L"4/4") && wcsstr(detail,L"回读"));
    // Run the actual worker through a failed first battery read, then its timer
    // expiry and successful automatic read; no ForceRefresh/right-click call.
    identityOk=true;radio=false;failPower=true;Keyboard::WorkerClock=FakeClock;
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
    Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    assert(Keyboard::stopEvent && Keyboard::refreshEvent);Keyboard::Worker(nullptr);
    // Original independent USB power/configuration deadlines are restored.
    assert(observedWaits==std::vector<DWORD>({3000,2000,1000}) && observedBattery==std::vector<bool>({false,false,true}));
    assert(Keyboard::GetCurrentState().batteryValid && Keyboard::GetCurrentState().battery==100);
    Keyboard::Stop();
    // Diagnosis uses the original 3s cadence and enumerates each refresh.
    for(unsigned scenario:{1u,2u}) {
        pressureScenario=scenario;radio=false;failPower=false;workerTime=0;observedWaits.clear();
        {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
        before=configWrites;int pressurePower=powerQueries,pressureConfig=configurationQueries;
        Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Keyboard::Worker(nullptr);
        assert(configWrites==before && powerQueries==pressurePower+(scenario==2?2:1) && configurationQueries==pressureConfig+3);
        ResetEvent(Keyboard::stopEvent);auto context=Keyboard::GetCurrentState();int scans=enumerateCalls;
        assert(Keyboard::QueryLightingUnlocked(context).lightingValid && enumerateCalls==scans+1);
        ChangeGeneration();context=Keyboard::GetCurrentState();
        assert(Keyboard::QueryLightingUnlocked(context).lightingValid && enumerateCalls==scans+2);
        identityOk=false;assert(!Keyboard::QueryLightingUnlocked(context).lightingValid && enumerateCalls==scans+3);
        identityOk=true;assert(Keyboard::QueryLightingUnlocked(context).lightingValid && enumerateCalls==scans+4 && configWrites==before);
        Keyboard::Stop();
    }
    pressureScenario=0;
    assert(Keyboard::LightingRefreshDelay(true,0)==3000);
    assert(Keyboard::RefreshDelay(false,1)==5000);
    // External hotkey-equivalent config changes publish one typed event.
    observedWaits.clear();observedBattery.clear();workerTime=0;waitsToStop=2;simulateChanges=true;page[76]=4;page[77]=0x40;
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
    before=configWrites;int beforePower=powerQueries;
    Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Keyboard::Worker(nullptr);
    assert(changedMask==(Keyboard::BrightnessChanged|Keyboard::SpeedChanged));
    assert(changedRevision==LPARAM(Keyboard::GetCurrentState().lightingRevision) && changedRevision==2);
    assert(configWrites==before && observedBattery.size()==2 && powerQueries==beforePower+1);
    assert(observedWaits==std::vector<DWORD>({3000,3000}));
    // Re-reading a setting applied through the menu must not notify again.
    auto same=Keyboard::GetCurrentState();assert(Keyboard::LightingChanges(same,same)==0);
    Keyboard::Stop();
    // Failed fast reads retreat independently; the regular battery poll does
    // not reset or bypass the configuration retry deadline.
    observedWaits.clear();observedBattery.clear();workerTime=0;waitsToStop=10;simulateChanges=false;failConfigRead=true;changedMask=0;
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
    before=configWrites;beforePower=powerQueries;int beforeConfig=configurationQueries;
    Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Keyboard::Worker(nullptr);
    assert(observedWaits==std::vector<DWORD>({15000,45000,15000,45000,60000,60000,60000,60000,15000,45000}));
    assert(powerQueries==beforePower+7 && configurationQueries==beforeConfig+4 && configWrites==before && changedMask==0);
    assert(Keyboard::GetCurrentState().batteryValid && !Keyboard::GetCurrentState().lightingValid);
    failConfigRead=false;Keyboard::Stop();
    // In-flight invalidation waits before retry; target PnP bursts coalesce,
    // and even a real target event cannot reset a failed config read's backoff.
    observedWaits.clear();observedBattery.clear();workerTime=0;waitsToStop=2;generationOnRead=true;
    {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
    before=configWrites;beforePower=powerQueries;beforeConfig=configurationQueries;
    Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Keyboard::Worker(nullptr);
    assert(observedWaits==std::vector<DWORD>({1000,3000}) && powerQueries==beforePower+2 && configurationQueries==beforeConfig+2 && configWrites==before);
    Keyboard::Stop();
    for(unsigned scenario:{1u,2u}) {
        pnpScenario=scenario;pnpWait=0;workerTime=0;failConfigRead=scenario==2;
        pnpConfig=configurationQueries;pnpPower=powerQueries;before=configWrites;
        {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
        Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Keyboard::Worker(nullptr);assert(configWrites==before);Keyboard::Stop();
    }
    pnpScenario=0;failConfigRead=false;
    Keyboard::stopEvent=CreateEventW(nullptr,TRUE,TRUE,nullptr);assert(Keyboard::stopEvent);
    before=configWrites;beforeConfig=configurationQueries;
    for(bool wireless:{false,true}){radio=wireless;assert(Keyboard::SetLighting(false,2)==Keyboard::LightingResult::Superseded);}
    assert(configWrites==before && configurationQueries==beforeConfig);Keyboard::Stop();
    // Actual worker: bursts settle before one read, delayed query echoes cannot
    // create a loop, a failed hint read falls back, and lost hints retain the 3s USB fallback.
    radio=false;
    for(unsigned scenario:{1u,2u,3u,4u,5u,6u}) {
        notificationScenario=scenario;notificationWait=0;workerTime=0;page[76]=4;changedMask=0;
        notificationBaselineQueries=configurationQueries;beforePower=powerQueries;before=configWrites;int beforeEnumerate=enumerateCalls;
        {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
        Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Keyboard::Worker(nullptr);
        assert(configWrites==before && powerQueries==beforePower+(scenario==4?2:1));
        if(scenario==1)assert(enumerateCalls==beforeEnumerate+3);
        if(scenario==2)assert(Keyboard::GetCurrentState().lightingStale && !changedMask);
        else if(scenario==4)assert(Keyboard::GetCurrentState().brightness==4 && !changedMask);
        else assert(Keyboard::GetCurrentState().brightness==3 && changedMask==Keyboard::BrightnessChanged);
        failConfigRead=false;Keyboard::Stop();assert(notificationOpens==notificationCloses);
    }
    notificationScenario=0;
    // Radio07/02 drive one settled full refresh without racing query/ACK input.
    // Cover malformed frames, no-op echoes, loss, mode generation changes and
    // public read/set transactions pausing the same reader with a stable event.
    for(unsigned scenario:{1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u}) {
        radioScenario=scenario;radioWait=0;radio=true;workerTime=0;page[76]=4;changedMask=0;
        radioBaselineConfig=configurationQueries;radioBaselinePower=powerQueries;before=configWrites;
        {std::lock_guard<std::mutex> lock(Keyboard::mutex);Keyboard::state=Keyboard::State{};}
        Keyboard::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);Keyboard::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Keyboard::Worker(nullptr);
        assert(configWrites==before);
        const int expectedPower=(scenario==3 || scenario==5 || scenario==6 || scenario==10 || scenario==12 || scenario==13)?1:(scenario==7 || scenario==8?3:2);
        assert(powerQueries==radioBaselinePower+expectedPower);
        assert(configurationQueries==radioBaselineConfig+(scenario==10?1:(scenario==7 || scenario==8 || scenario==9)?3:2));
        if(scenario==2)assert(Keyboard::GetCurrentState().lightingStale && !changedMask);
        else if(scenario==10)assert(!Keyboard::GetCurrentState().isConnected && !Keyboard::GetCurrentState().batteryValid && !Keyboard::GetCurrentState().lightingValid && !Keyboard::GetCurrentState().sleepValid && !changedMask);
        else if(scenario==4 || scenario==9 || scenario==11)assert(!changedMask);
        else assert(Keyboard::GetCurrentState().brightness==3);
        failConfigRead=false;identityOk=true;Keyboard::Stop();assert(!radioReaderActive && !Keyboard::radioInput.Event());
    }
    radioScenario=0;radio=false;
    // Hint budget, malformed/short packets, missing numeric values, harmless
    // query echoes, bounded burst delay and a new connection's recovery policy.
    const BYTE hint[]={3,8,0,1},wrongHint[]={3,8,0,2};
    KeyboardNotifications::Policy policy;policy.Reset(0);
    assert(!policy.Hint(hint,4,100) && !policy.Hint(hint,3,500) && !policy.Hint(wrongHint,4,500));
    for(unsigned i=0;i<4;++i) {
        auto time=ULONGLONG(1000+i*3000);assert(policy.Hint(hint,4,time));
        assert(!policy.Ready(time+999) && policy.Ready(time+1000));
        policy.Read(true,true,true,time+1000);
    }
    assert(!policy.Hint(hint,4,14000));assert(policy.Hint(hint,4,61000));
    assert(policy.Hint(hint,4,61900) && policy.due==62900);
    assert(policy.Hint(hint,4,62800) && policy.due==63000); //2s maximum burst delay
    policy.Read(true,false,true,63000);assert(!policy.enabled && !policy.pending);
    assert(!policy.Hint(hint,4,64000));policy.Reset(65000);
    assert(policy.Hint(hint,4,66000));policy.Read(false,true,true,67000);assert(!policy.enabled);
    policy.Reset(70000);
    for(unsigned i=0;i<257;++i)policy.Hint(hint,4,71000+i);
    assert(!policy.enabled && !policy.pending);
    puts("PASS: keyboard USB/radio transactions, radio07/02 bounded refresh and validated disconnect, rejected connection values, unchanged battery timestamps, independent polling/backoff, external changes and popup formatting; fake HID only");
}
