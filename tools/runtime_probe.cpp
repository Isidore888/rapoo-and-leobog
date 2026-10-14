#include "../src/device_manager.h"
#include "../src/keyboard_manager.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
static void Changed(const Device::State&,DWORD) {}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    bool passive=argc>2 && strcmp(argv[2],"--passive")==0;
    if(!Device::Start(nullptr,Changed,!passive))return 2;
    if(!Keyboard::Start(nullptr,WM_APP+2)){Device::Stop();return 2;}
    Sleep(argc>1?std::clamp(atoi(argv[1]),1,90)*1000:4000);
    auto mouse=Device::GetCurrentState();
    auto keyboard=Keyboard::GetCurrentState();
    printf("mouse connected=%d wired=%d valid=%d percent=%d charging=%d dpi=%d polling=%d sleep=%d\n",mouse.isConnected,mouse.isWired,mouse.batteryValid,mouse.battery,mouse.isCharging,mouse.dpiX,mouse.pollingHz,mouse.sleepMinutes);
    printf("keyboard connected=%d wired=%d valid=%d percent=%d charging=%d chargingKnown=%d\n",keyboard.isConnected,keyboard.isWired,keyboard.batteryValid,keyboard.battery,keyboard.isCharging,keyboard.chargingKnown);
    Keyboard::Stop();Device::Stop();return mouse.batteryValid&&keyboard.batteryValid?0:1;
}
