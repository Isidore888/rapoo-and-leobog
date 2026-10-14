#pragma once
#include <windows.h>
extern "C" {
#include <hidsdi.h>
}
#include <string>
#include <vector>
namespace Hid {
struct Endpoint {
    std::wstring path;
    USHORT vid = 0, pid = 0;
    HIDP_CAPS caps{};
};
std::vector<Endpoint> Enumerate();
HANDLE Open(const Endpoint& ep, bool overlapped);
bool Transfer(HANDLE handle, BYTE* data, DWORD size, bool write, DWORD timeout, DWORD* received = nullptr);
bool Feature(HANDLE handle, BYTE* data, DWORD size, bool write, DWORD timeout, HANDLE stop = nullptr, DWORD* received = nullptr);
}
