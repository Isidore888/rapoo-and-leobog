#pragma once
#include <windows.h>
#include "device_manager.h"
#include "keyboard_manager.h"
#include "alert_policy.h"

namespace Alert {

void Init(HINSTANCE hInstance);
void Cleanup();
void CheckBattery(const Device::State& mouse, const Keyboard::State& keyboard);
AlertPolicy::Config GetConfig(int device);
bool SaveConfig(const AlertPolicy::Config (&config)[2]);
void ShowSettings(HWND owner);
void CloseSettings();
bool HandleSettingsMessage(MSG* message);
void Show(const WCHAR* modelName, int battery, bool isCritical, bool isWired);
void Hide();
void DrawAlert(HDC dc,int dpi,const wchar_t* model,int battery,bool critical,bool buttonHover=false,bool closeHover=false);

} // namespace Alert
