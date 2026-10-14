#pragma once
#include <windows.h>
#include <string>

namespace Autostart {
enum class Status { Disabled, Registered, InvalidPath, Unknown };
struct State { Status status=Status::Unknown; DWORD error=ERROR_SUCCESS; bool approvalPresent=false; bool hasRegistration=false; };
// Registered means a valid own Run command AND a recognized enabled approval.
// Disabled may retain the Run command. Unknown approval formats are never changed.
State Read();
State Set(bool enabled);
const wchar_t* Label(const State& state);
std::wstring Command(const std::wstring& executable);
}
