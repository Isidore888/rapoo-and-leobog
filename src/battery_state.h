#pragma once
#include <windows.h>
#include <cwchar>
namespace Battery {
constexpr ULONGLONG FreshnessMs = 180000;
inline bool Fresh(bool valid, ULONGLONG updated, ULONGLONG now = GetTickCount64()) {
    return valid && now >= updated && now - updated <= FreshnessMs;
}
inline const wchar_t* Description(bool connected, bool valid, int percent, bool charging, bool wired, wchar_t* out, size_t size) {
    if (!connected) return L"休眠 / 未连接";
    if (!valid || percent < 0 || percent > 100) return L"电量未知 / 已过期";
    swprintf_s(out, size, L"%d%% · %s%s", percent, wired ? L"USB" : L"2.4G", charging ? L" · 充电中" : L"");
    return out;
}
}
