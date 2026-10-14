#pragma once
#include <cstddef>
#include <cstdint>
#include <algorithm>
namespace KeyboardProtocol {
using Byte = std::uint8_t;
constexpr Byte Signature[] = {3, 0, 0, 0, 1, 0x21};
constexpr std::size_t LightingSize=128;
struct Lighting { int effect=-1,brightness=-1,speed=-1; };
enum class Setting { Brightness, Speed, Sleep };
inline bool ConfigPage(const Byte* p,std::size_t n) {
    return p && n==LightingSize && p[126]==0x5A && p[127]==0xA5 && p[56]==0xFF && p[57]==0xFF;
}
inline bool ParseSleep(const Byte* p,std::size_t n,int& seconds) {
    if(!ConfigPage(p,n) || p[24]<1 || p[24]>240)return false;
    seconds=p[24]*30;return true;
}
inline bool PatchSleep(Byte* p,std::size_t n,int seconds) {
    if(!ConfigPage(p,n) || seconds<30 || seconds>7200 || seconds%30)return false;
    p[24]=Byte(seconds/30);return true;
}
inline bool ParseLighting(const Byte* p,std::size_t n,Lighting& out) {
    if(!ConfigPage(p,n) || p[10]<1 || p[10]>18)return false;
    const auto at=58+2*(p[10]-1);
    if(p[at]>4 || (p[at+1]>>4)>4)return false;
    out={p[10],p[at],p[at+1]>>4};return true;
}
inline bool PatchLighting(Byte* p,std::size_t n,bool speed,int level) {
    Lighting current;if(level<0 || level>4 || !ParseLighting(p,n,current))return false;
    const auto at=58+2*(current.effect-1);
    if(speed)p[at+1]=Byte((level<<4)|(p[at+1]&15));else p[at]=Byte(level);
    return true;
}
inline bool WirelessFrame(const Byte* p,std::size_t n,Byte command) {
    if(!p || n!=20 || p[0]!=0x13 || p[1]!=command)return false;
    Byte sum=0;for(int i=0;i<19;++i)sum+=p[i];return sum==p[19];
}
inline bool LightingFrame(const Byte* p,std::size_t n) {
    return WirelessFrame(p,n,0x44) && p[2]==10 && p[3]<10 && p[4]==(p[3]==9?2:14);
}
inline void LightingWritePage(const Byte* page,int index,Byte* out) {
    std::fill(out,out+20,0);out[0]=0x13;out[1]=4;out[2]=10;out[3]=Byte(index);out[4]=index==9?2:14;
    std::copy(page+index*14,page+index*14+out[4],out+5);
    for(int i=0;i<19;++i)out[19]+=out[i];
}
inline bool LightingWriteAck(const Byte* reply,std::size_t n,const Byte* request) {
    // HI98C Pro echoes the complete page, including payload and checksum.
    return WirelessFrame(reply,n,4) && std::equal(reply,reply+20,request);
}
struct Power { int percent = -1; bool charging = false; bool chargingKnown = false; };
inline bool WiredHeader(const Byte* p, std::size_t n, Byte command, Byte parameter, Byte length) {
    return p && n >= std::size_t(8 + length) && p[0] == 6 && p[1] == command && p[2] == parameter && p[3] == 0 && p[4] == 1 && p[5] == 0 && p[6] == length && p[7] == 0;
}
inline bool WiredIdentity(const Byte* p, std::size_t n) {
    return WiredHeader(p, n, 0x82, 1, 6) && std::equal(Signature, Signature + 6, p + 8);
}
inline bool WiredPower(const Byte* p, std::size_t n, Power& out) {
    if (!WiredHeader(p, n, 0x87, 2, 2) || p[8] > 100) return false;
    out = {p[8], (p[9] & 0xF0) != 0 && (p[9] & 0x0F) == 0, true}; return true;
}
inline bool WirelessHeader(const Byte* p, std::size_t n, Byte command, Byte length) {
    if (!p || n != 20 || p[0] != 0x13 || p[1] != command || p[2] != 1 || p[3] != 0 || p[4] != length) return false;
    Byte sum = 0; for (int i = 0; i < 19; ++i) sum += p[i];
    return sum == p[19];
}
inline bool WirelessIdentity(const Byte* p, std::size_t n) {
    return WirelessHeader(p, n, 5, 10) && std::equal(Signature, Signature + 6, p + 5);
}
inline bool WirelessPower(const Byte* p, std::size_t n, Power& out) {
    if (!WirelessHeader(p, n, 0x0A, 4) || p[5] != 5 || p[7] != 1 || p[6] > 100) return false;
    // Wireless charge flags are not established for this hardware.
    out = {p[6], false, false}; return true;
}
inline bool WirelessChangeHint(const Byte* p,std::size_t n) {
    // Exact zero-request capture, correlated with changed configuration after
    // settling. This is only a hint: no battery/light/sleep numbers live here.
    return WirelessHeader(p,n,0x0A,4) && p[5]==7 &&
        std::all_of(p+6,p+19,[](Byte byte){return byte==0;});
}
inline bool WirelessConnectionHint(const Byte* p,std::size_t n) {
    // Captured offline/online events trigger a verified read, not cached values.
    return WirelessHeader(p,n,0x0A,4) && p[5]==2 && p[6]<=1 &&
        std::all_of(p+7,p+19,[](Byte byte){return byte==0;});
}
inline void WiredRequest(Byte command, Byte parameter, Byte length, Byte* p) {
    std::fill(p, p + 520, 0); p[0] = 6; p[1] = command; p[2] = parameter; p[4] = 1; p[6] = length;
}
inline void WirelessRequest(Byte command, Byte* p) {
    std::fill(p, p + 20, 0); p[0] = 0x13; p[1] = command; p[2] = 1;
    p[19] = Byte(p[0] + p[1] + p[2]);
}
}
