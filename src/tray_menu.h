#pragma once
#include <windows.h>
#include <shellapi.h>
#include "device_manager.h"
#include "keyboard_manager.h"

namespace Tray {
constexpr UINT KeyboardSettingMessage=WM_APP+5;
constexpr UINT MouseSettingMessage=WM_APP+8; // 0: polling, 1: sleep; +6 belongs to keyboard changes

void InitTheme();
void ShowMenu(HWND hWndOwner);
void DismissAllMenus();
void UpdateMenuState(const Device::State& mouse,const Keyboard::State& keyboard);
void SetSettingPending(int submenuType,bool pending);
SIZE DrawMenuPreview(HDC dc,int dpi,bool dark,const Device::State& mouse,const Keyboard::State& keyboard);

HICON CreateBatteryIcon(int battery, bool isCharging, bool isConnected, int size, bool isDark, int keyboardBattery = -1, bool keyboardCharging = false, bool keyboardValid = false, int styleOverride = -1);
void UpdateTooltip(NOTIFYICONDATAW& nid, const Device::State& state, const Keyboard::State& keyboard);

int CycleBatteryStyle();
int GetBatteryStyle();

bool IsAutoRunEnabled();
void ToggleAutoRun();

} // namespace Tray
