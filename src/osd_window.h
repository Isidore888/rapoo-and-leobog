#pragma once
#include <windows.h>
#include "device_manager.h"
#include "keyboard_manager.h"

namespace Osd {

bool Init(HINSTANCE hInstance);
void Cleanup();

// Shows custom 2-line or 3-line notification with auto-sizing geometry
void Show(const WCHAR* line1, const WCHAR* line2, const WCHAR* line3 = nullptr, UINT durationMs = 1600);
void ShowValue(const wchar_t* label,const wchar_t* value,bool keyboard,UINT durationMs=1600);
void DrawValue(HDC dc,int dpi,const wchar_t* label,const wchar_t* value,bool keyboard);

// Formatted notification when DPI changes
void ShowDpiUpdate(int level, int dpix, int dpiy, int battery, int pollingHz, bool isCharging, bool isWired, const WCHAR* modelName);

HWND GetWindowHandle();
void ShowStatus(const Device::State& mouse, const Keyboard::State& keyboard, POINT anchor, UINT durationMs=4000);
void UpdateStatus(const Device::State& mouse, const Keyboard::State& keyboard);
void HideStatus();
bool IsStatusVisible();
void DrawStatus(HDC dc, int dpi, bool dark, const Device::State& mouse, const Keyboard::State& keyboard);

} // namespace Osd
