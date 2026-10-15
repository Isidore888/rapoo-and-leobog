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
// 0: all supported VIDs; 0x24AE: mouse only; 0x258A/0x3554: keyboard only.
inline thread_local USHORT filterVid = 0;
struct ScopedVidFilter {
    USHORT previous = filterVid;
    explicit ScopedVidFilter(USHORT vid) { filterVid = vid; }
    ~ScopedVidFilter() { filterVid = previous; }
};
std::vector<Endpoint> Enumerate();
HANDLE Open(const Endpoint& ep, bool overlapped);
bool Transfer(HANDLE handle, BYTE* data, DWORD size, bool write, DWORD timeout, DWORD* received = nullptr);
bool Feature(HANDLE handle, BYTE* data, DWORD size, bool write, DWORD timeout, HANDLE stop = nullptr, DWORD* received = nullptr);
}
