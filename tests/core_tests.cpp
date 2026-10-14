#include "../src/keyboard_protocol.h"
#include "../src/alert_policy.h"
#include "../src/rapoo_protocol.h"
#include "../src/battery_state.h"
#include "../src/keyboard_manager.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
    Keyboard::State baseline;baseline.isConnected=baseline.lightingValid=true;baseline.lastLightingUpdate=1000;
    baseline.lightEffect=10;baseline.brightness=baseline.lightSpeed=4;
    auto updated=baseline;updated.brightness=3;updated.lastLightingUpdate=2000;
    assert(Keyboard::LightingChanges(baseline,updated,2000)==Keyboard::BrightnessChanged);
    updated.lightSpeed=2;assert(Keyboard::LightingChanges(baseline,updated,2000)==(Keyboard::BrightnessChanged|Keyboard::SpeedChanged));
    assert(Keyboard::LightingChanges(updated,updated,2000)==0);
    auto invalid=baseline;invalid.lightingValid=false;assert(Keyboard::LightingChanges(invalid,updated,2000)==0);
    invalid=updated;invalid.isWired=true;assert(Keyboard::LightingChanges(baseline,invalid,2000)==0);
    invalid=updated;invalid.lightEffect=11;assert(Keyboard::LightingChanges(baseline,invalid,2000)==0);
    invalid=baseline;invalid.isConnected=false;assert(Keyboard::LightingChanges(invalid,updated,2000)==0);
    assert(Keyboard::LightingChanges(baseline,updated,1000+Battery::FreshnessMs+1)==0);
    assert(Keyboard::LightingChanges(Keyboard::State{},updated,2000)==0);
    BYTE light[128]{};light[10]=10;light[56]=light[57]=0xFF;light[126]=0x5A;light[127]=0xA5;light[76]=3;light[77]=0x30;
    KeyboardProtocol::Lighting lighting;
    assert(KeyboardProtocol::ParseLighting(light,128,lighting) && lighting.effect==10 && lighting.brightness==3 && lighting.speed==3);
    BYTE unchanged[128];std::copy(light,light+128,unchanged);
    assert(KeyboardProtocol::PatchLighting(light,128,false,2));
    for(int i=0;i<128;++i)assert(light[i]==(i==76?2:unchanged[i]));
    light[77]=0x37;std::copy(light,light+128,unchanged);
    assert(KeyboardProtocol::PatchLighting(light,128,true,1) && light[77]==0x17);
    for(int i=0;i<128;++i)if(i!=77)assert(light[i]==unchanged[i]);
    assert(!KeyboardProtocol::PatchLighting(light,128,true,5));light[10]=0;
    assert(!KeyboardProtocol::ParseLighting(light,128,lighting));light[10]=10;light[127]=0;
    assert(!KeyboardProtocol::ParseLighting(light,128,lighting));
    using namespace KeyboardProtocol;
    Byte usb[520] = {6,0x82,1,0,1,0,6,0,3,0,0,0,1,0x21};
    assert(WiredIdentity(usb,520));assert(!WiredIdentity(usb,13));
    usb[13]=0x22;assert(!WiredIdentity(usb,520));usb[13]=0x21;
    Byte power[520]={6,0x87,2,0,1,0,2,0,100,0x10};Power out;
    assert(WiredPower(power,520,out)&&out.percent==100&&out.charging&&out.chargingKnown);
    power[8]=0;assert(WiredPower(power,520,out)&&out.percent==0);
    power[8]=73;power[9]=0;assert(WiredPower(power,520,out)&&out.percent==73&&!out.charging);
    power[8]=101;assert(!WiredPower(power,520,out));power[8]=20;
    power[1]=0x88;assert(!WiredPower(power,520,out));power[1]=0x87;
    power[2]=1;assert(!WiredPower(power,520,out));power[2]=2;assert(!WiredPower(power,9,out));
    Byte id[20]={0x13,5,1,0,10,3,0,0,0,1,0x21,1,0,0,0,0,0,0,0,0x49};
    assert(WirelessIdentity(id,20));id[19]++;assert(!WirelessIdentity(id,20));id[19]--;
    Byte radio[20]={0x13,0x0A,1,0,4,5,100,1,0,0,0,0,0,0,0,0,0,0,0,0x8C};
    assert(WirelessPower(radio,20,out)&&out.percent==100&&!out.chargingKnown);
    auto checksum=[&](){radio[19]=0;for(int i=0;i<19;++i)radio[19]+=radio[i];};
    for(Byte value:{Byte(0),Byte(1),Byte(20),Byte(87)}){radio[6]=value;checksum();assert(WirelessPower(radio,20,out)&&out.percent==value);}
    radio[6]=101;checksum();assert(!WirelessPower(radio,20,out));radio[6]=40;radio[7]=0;checksum();assert(!WirelessPower(radio,20,out));
    radio[7]=1;radio[1]=0x0B;checksum();assert(!WirelessPower(radio,20,out));assert(!WirelessPower(radio,19,out));
    // 2026-10-12 zero-request radio observation: valid frame, unknown event,
    // not the validated power reply. Never reinterpret its zero bytes as 0%.
    const Byte unsolicited[20]={0x13,0x0A,1,0,4,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0x29};
    assert(WirelessFrame(unsolicited,20,0x0A) && !WirelessPower(unsolicited,20,out));
    assert(!LightingFrame(unsolicited,20));
    assert(WirelessChangeHint(unsolicited,20) && !WirelessChangeHint(unsolicited,19));
    Byte badHint[20];std::copy(unsolicited,unsolicited+20,badHint);
    badHint[19]^=1;assert(!WirelessChangeHint(badHint,20));
    std::copy(unsolicited,unsolicited+20,badHint);badHint[5]=5;badHint[19]-=2;
    assert(!WirelessChangeHint(badHint,20) && !WirelessPower(badHint,20,out));
    std::copy(unsolicited,unsolicited+20,badHint);badHint[7]=1;++badHint[19];
    assert(!WirelessPower(badHint,20,out) && !WirelessChangeHint(badHint,20));
    // 2026-10-13 zero-query connection captures: 02/0 and 02/1, not battery.
    Byte connection[20]={0x13,0x0A,1,0,4,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0x24};
    assert(WirelessConnectionHint(connection,20) && !WirelessPower(connection,20,out));
    connection[6]=1;connection[19]=0x25;
    assert(WirelessConnectionHint(connection,20) && !WirelessChangeHint(connection,20));
    assert(!WirelessConnectionHint(nullptr,20) && !WirelessConnectionHint(connection,19));
    connection[19]^=1;assert(!WirelessConnectionHint(connection,20));connection[19]^=1;
    connection[6]=2;connection[19]=0x26;assert(!WirelessConnectionHint(connection,20));
    connection[6]=1;connection[7]=1;assert(!WirelessConnectionHint(connection,20));
    connection[7]=0;connection[2]=2;connection[19]=0x26;assert(!WirelessConnectionHint(connection,20));
    BYTE mouse[19]={7,0x40,1,0x20,3,0x20,3,1,100,0,4,1,1,0x6E,0x46,0x10,2,4,0xB4};Rapoo::DeviceStatus status;
    assert(Rapoo::ParseStatusReport(mouse,19,status,-1)&&status.battery==100&&!status.isCharging&&status.dpiX==800&&status.pollingHz==1000&&status.profileSlot==1);
    mouse[11]=0x84;assert(Rapoo::ParseStatusReport(mouse,19,status,-1)&&status.pollingHz==2000);
    mouse[11]=0xFF;mouse[12]=8;assert(Rapoo::ParseStatusReport(mouse,19,status,-1)&&status.pollingHz==0&&status.profileSlot==-1);
    mouse[11]=1;mouse[12]=1;
    mouse[1]=0x42;assert(Rapoo::ParseStatusReport(mouse,19,status,-1)&&status.isCharging);
    mouse[8]=0;assert(Rapoo::ParseStatusReport(mouse,19,status,100)&&status.battery==0);
    mouse[8]=1;assert(Rapoo::ParseStatusReport(mouse,19,status,100)&&status.battery==1);
    mouse[8]=255;assert(!Rapoo::ParseStatusReport(mouse,19,status,100));mouse[8]=20;mouse[1]=0x50;assert(!Rapoo::ParseStatusReport(mouse,19,status,100));
    assert(!Rapoo::ParseStatusReport(mouse,8,status,100));assert(!Rapoo::ParseStatusReport(mouse,18,status,100));
    using namespace AlertPolicy;Config config;Tracker m,k;
    BYTE bank=0;assert(Rapoo::ProfileBank(0,bank)&&bank==8);assert(Rapoo::ProfileBank(1,bank)&&bank==12);assert(Rapoo::ProfileBank(7,bank)&&bank==36);assert(!Rapoo::ProfileBank(8,bank));
    assert(Rapoo::CodeToPollingHz(0x84)==2000&&Rapoo::CodeToPollingHz(0x01)==1000&&Rapoo::CodeToPollingHz(0xFF)==0);
    BYTE command[33]{},setting=1;
    for(bool wired:{false,true}) {
        BYTE prefix=wired?0xFF:0xA5;
        assert(Rapoo::BuildReadCommand(12,0x80,1,command,33,wired)==33&&command[1]==prefix&&command[2]==0xA4);
        assert(Rapoo::BuildWriteCommand(12,0x80,&setting,1,command,33,wired)==33&&command[1]==prefix&&command[2]==0xA5&&command[8]==1);
        assert(Rapoo::BuildUnlockPacket(command,33,wired)==33&&command[1]==prefix&&command[2]==0xA3);
    }
    auto eval=[&](Tracker& t,int p,bool charge=false,bool fresh=true,bool conn=true){return t.Evaluate(config,conn,fresh,p,charge);};
    assert(eval(m,21)==0);assert(eval(m,20)==1);assert(eval(m,19)==0);assert(eval(m,10)==2);assert(eval(m,0)==0);
    assert(eval(k,5)==2);assert(eval(k,5)==0); // independent tracker and no warning duplicate
    assert(eval(m,24)==0);assert(eval(m,20)==0);assert(eval(m,25,true)==0);assert(eval(m,20)==1);
    Tracker guarded;assert(eval(guarded,10,true)==0);assert(eval(guarded,10,false,false)==0);assert(eval(guarded,10,false,true,false)==0);assert(eval(guarded,10)==2);
    config.enabled=false;Tracker disabled;assert(eval(disabled,5)==0);config.enabled=true;assert(eval(disabled,5)==2);
    assert(!Valid({true,10,10})&&!Valid({true,20,0})&&!Valid({true,101,10})&&Valid({true,100,99}));
    Config maximum{true,100,99};Tracker full;
    assert(full.Evaluate(maximum,true,true,100,false)==1);
    for(int i=0;i<10;++i)assert(full.Evaluate(maximum,true,true,100,false)==0);
    assert(full.Evaluate(maximum,true,true,99,false)==2);
    assert(full.Evaluate(maximum,true,true,100,true)==0);
    assert(full.Evaluate(maximum,true,true,100,false)==1);
    assert(full.Evaluate(maximum,true,true,100,false)==0);
    assert(Battery::Fresh(true,1000,181000));assert(!Battery::Fresh(true,1000,181001));assert(!Battery::Fresh(false,1000,1100));
    puts("PASS: captured reports, battery boundaries, identity/checksum rejection, independent alerts, hysteresis and freshness");
}
