#include <windows.h>
#include <cassert>
#include <string>
#include <vector>
#include <cstdio>
static bool registered=false,approval=false,failRead=false,failApprovalRead=false;
static bool failRunWrite=false,failApprovalWrite=false,failApprovalDelete=false;
static bool loseRunWrite=false,loseApprovalWrite=false,failApprovalCreate=false;
static DWORD kind=REG_SZ,approvalKind=REG_BINARY,mutations=0,runWrites=0,approvalWrites=0,approvalDeletes=0;
static std::vector<BYTE> value,approvalValue;
static std::wstring executable=L"H:\\Work project\\rapoo-and-leobog\\bin\\rapoo-tray.exe";
static void Approved(BYTE code) {approval=true;approvalKind=REG_BINARY;approvalValue.assign(12,0);approvalValue[0]=code;}
static bool IsApproval(LPCWSTR path) {
    assert(!wcscmp(path,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") ||
           !wcscmp(path,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"));
    return wcsstr(path,L"StartupApproved")!=nullptr;
}
static LSTATUS WINAPI Open(HKEY root,LPCWSTR path,DWORD,REGSAM access,PHKEY out) {
    assert(root==HKEY_CURRENT_USER && (access==KEY_QUERY_VALUE || access==KEY_SET_VALUE));
    bool approved=IsApproval(path);
    if(failRead || (approved && failApprovalRead))return ERROR_ACCESS_DENIED;
    if(approved && !approval)return ERROR_FILE_NOT_FOUND;
    *out=reinterpret_cast<HKEY>(approved?2:1);return ERROR_SUCCESS;
}
static LSTATUS WINAPI Query(HKEY key,LPCWSTR name,LPDWORD,LPDWORD type,LPBYTE data,LPDWORD size) {
    assert(!wcscmp(name,L"rapoo-tray"));
    const bool approved=key==reinterpret_cast<HKEY>(2);
    assert(approved || key==reinterpret_cast<HKEY>(1));
    if((approved && !approval) || (!approved && !registered))return ERROR_FILE_NOT_FOUND;
    const auto& bytes=approved?approvalValue:value;
    if(type)*type=approved?approvalKind:kind;
    if(!data){*size=DWORD(bytes.size());return ERROR_SUCCESS;}
    if(*size<bytes.size()){*size=DWORD(bytes.size());return ERROR_MORE_DATA;}
    if(!bytes.empty())CopyMemory(data,bytes.data(),bytes.size());
    *size=DWORD(bytes.size());return ERROR_SUCCESS;
}
static LSTATUS WINAPI Create(HKEY root,LPCWSTR path,DWORD,LPWSTR,DWORD,REGSAM access,const LPSECURITY_ATTRIBUTES,PHKEY out,LPDWORD) {
    assert(root==HKEY_CURRENT_USER && access==KEY_SET_VALUE);
    const bool approved=IsApproval(path);
    if(approved && failApprovalCreate)return ERROR_ACCESS_DENIED;
    *out=reinterpret_cast<HKEY>(approved?2:1);return ERROR_SUCCESS;
}
static LSTATUS WINAPI Set(HKEY key,LPCWSTR name,DWORD,DWORD type,const BYTE* data,DWORD size) {
    assert(!wcscmp(name,L"rapoo-tray"));++mutations;
    if(key==reinterpret_cast<HKEY>(2)) {
        assert(type==REG_BINARY && size==12 && (data[0]==0 || data[0]==1 || data[0]==3));
        assert(!data[1] && !data[2] && !data[3]);++approvalWrites;
        if(failApprovalWrite)return ERROR_ACCESS_DENIED;
        if(!loseApprovalWrite){approval=true;approvalKind=type;approvalValue.assign(data,data+size);}
    } else {
        assert(key==reinterpret_cast<HKEY>(1) && type==REG_SZ);++runWrites;
        if(failRunWrite)return ERROR_ACCESS_DENIED;
        if(!loseRunWrite){registered=true;kind=type;value.assign(data,data+size);}
    }
    return ERROR_SUCCESS;
}
static LSTATUS WINAPI Delete(HKEY key,LPCWSTR name) {
    // A toggle may clear only the own approval, never remove its Run registration.
    assert(key==reinterpret_cast<HKEY>(2) && !wcscmp(name,L"rapoo-tray"));++mutations;++approvalDeletes;
    if(failApprovalDelete)return ERROR_ACCESS_DENIED;
    approval=false;return ERROR_SUCCESS;
}
static LSTATUS WINAPI Close(HKEY){return ERROR_SUCCESS;}
static DWORD WINAPI Module(HMODULE,LPWSTR out,DWORD capacity) {
    assert(capacity>executable.size());wcscpy_s(out,capacity,executable.c_str());return DWORD(executable.size());
}
#define RegOpenKeyExW Open
#define RegQueryValueExW Query
#define RegCreateKeyExW Create
#define RegSetValueExW Set
#define RegDeleteValueW Delete
#define RegCloseKey Close
#define GetModuleFileNameW Module
#include "../src/autostart.cpp"
#undef RegOpenKeyExW
#undef RegQueryValueExW
#undef RegCreateKeyExW
#undef RegSetValueExW
#undef RegDeleteValueW
#undef RegCloseKey
#undef GetModuleFileNameW
int main() {
    using namespace Autostart;
    assert(Command(L"H:\\a b\\a.exe")==L"\"H:\\a b\\a.exe\"");
    assert(Command(L"").empty() && Command(L"a\"b").empty() && Command(std::wstring(259,L'a')).empty());
    assert(Read().status==Status::Disabled && !Read().hasRegistration);
    Approved(3);auto prior=mutations;assert(Set(false).status==Status::Disabled && mutations==prior);
    auto next=Set(true);assert(next.status==Status::Registered && next.hasRegistration && !approval && !next.error);
    assert(std::wstring(reinterpret_cast<const wchar_t*>(value.data()))==Command(executable));
    const auto originalCommand=value;auto initialRunWrites=runWrites;
    prior=mutations;assert(Set(true).status==Status::Registered && mutations==prior);
    next=Set(false);assert(next.status==Status::Disabled && next.hasRegistration && registered && next.approvalPresent);
    assert(value==originalCommand && runWrites==initialRunWrites && approvalValue[0]==1);
    prior=mutations;assert(Set(false).status==Status::Disabled && mutations==prior);
    next=Set(true);assert(next.status==Status::Registered && registered && value==originalCommand && approvalValue==std::vector<BYTE>(12,0));
    assert(!wcscmp(Label(next),L"✓ 已开启"));
    Approved(2);prior=mutations;assert(Read().status==Status::Registered && mutations==prior);
    assert(Set(false).status==Status::Disabled && approvalValue[0]==3 && value==originalCommand);
    assert(Set(true).status==Status::Registered && !approval && value==originalCommand);
    Approved(0);assert(Read().status==Status::Registered && Read().approvalPresent);
    // Native Windows off record captured on this machine, including its timestamp.
    Approved(1);approvalValue={1,0,0,0,0xC9,0x13,0xC2,0x97,0x92,0x59,0xDD,1};
    assert(Read().status==Status::Disabled && Read().hasRegistration && !Read().error);
    prior=mutations;assert(Set(false).status==Status::Disabled && mutations==prior);
    next=Set(true);assert(next.status==Status::Registered && approval && approvalValue==std::vector<BYTE>(12,0));
    assert(value==originalCommand && runWrites==initialRunWrites);
    next=Set(false);assert(next.status==Status::Disabled && approvalValue[0]==1);
    FILETIME disabledAt{};CopyMemory(&disabledAt,approvalValue.data()+4,sizeof(disabledAt));
    assert(disabledAt.dwLowDateTime || disabledAt.dwHighDateTime);
    assert(value==originalCommand && runWrites==initialRunWrites);
    assert(Set(true).status==Status::Registered && approvalValue==std::vector<BYTE>(12,0));
    // Simulate native Windows toggles in either direction; reading never writes.
    Approved(1);prior=mutations;assert(Read().status==Status::Disabled && mutations==prior);
    Approved(0);assert(Read().status==Status::Registered && mutations==prior);
    Approved(3);approvalValue[4]=0x77;assert(Read().status==Status::Disabled);
    assert(!wcscmp(Label(Read()),L"未开启"));assert(Set(true).status==Status::Registered);
    for(auto invalidCode:{BYTE(4),BYTE(6),BYTE(7),BYTE(255)}) {
        Approved(invalidCode);prior=mutations;
        assert(Read().status==Status::Unknown && Read().error==ERROR_NOT_SUPPORTED);
        assert(Set(true).error==ERROR_NOT_SUPPORTED && Set(false).error==ERROR_NOT_SUPPORTED && mutations==prior);
    }
    Approved(2);approvalValue[1]=1;prior=mutations;assert(Set(true).error==ERROR_NOT_SUPPORTED && mutations==prior);
    for(BYTE code=0;code<4;++code)for(unsigned reserved=1;reserved<4;++reserved) {
        Approved(code);approvalValue[reserved]=1;prior=mutations;
        assert(Set(true).error==ERROR_NOT_SUPPORTED && Set(false).error==ERROR_NOT_SUPPORTED && mutations==prior);
    }
    Approved(2);approvalValue.resize(11);assert(Read().status==Status::Unknown);
    Approved(2);approvalValue.resize(24);assert(Read().status==Status::Unknown);
    Approved(2);approvalKind=REG_SZ;assert(Read().status==Status::Unknown);
    approval=false;failApprovalRead=true;prior=mutations;
    assert(Set(false).error==ERROR_ACCESS_DENIED && mutations==prior);failApprovalRead=false;
    failApprovalCreate=true;assert(Set(false).error==ERROR_ACCESS_DENIED && registered && !approval);failApprovalCreate=false;
    failApprovalWrite=true;assert(Set(false).error==ERROR_ACCESS_DENIED && Read().status==Status::Registered);failApprovalWrite=false;
    loseApprovalWrite=true;assert(Set(false).error==ERROR_INVALID_DATA && Read().status==Status::Registered);loseApprovalWrite=false;
    assert(Set(false).status==Status::Disabled);failApprovalWrite=true;
    assert(Set(true).error==ERROR_ACCESS_DENIED && Read().status==Status::Disabled);failApprovalWrite=false;
    loseApprovalWrite=true;assert(Set(true).error==ERROR_INVALID_DATA && Read().status==Status::Disabled);loseApprovalWrite=false;
    assert(Set(true).status==Status::Registered && approvalValue==std::vector<BYTE>(12,0));
    Approved(3);failApprovalDelete=true;
    assert(Set(true).error==ERROR_ACCESS_DENIED && registered && Read().status==Status::Disabled);failApprovalDelete=false;
    assert(Set(true).status==Status::Registered);
    std::wstring unquoted=executable;for(auto& c:unquoted)c=towlower(c);
    value.resize((unquoted.size()+1)*sizeof(wchar_t));CopyMemory(value.data(),unquoted.c_str(),value.size());
    assert(Read().status==Status::InvalidPath);assert(Set(true).status==Status::Registered);
    executable=L"H:\\moved\\rapoo-tray.exe";assert(Read().status==Status::InvalidPath);
    Approved(255);prior=mutations;assert(Set(true).error==ERROR_NOT_SUPPORTED && mutations==prior);
    approval=false;assert(Set(true).status==Status::Registered);
    registered=false;failRunWrite=true;prior=approvalWrites;
    assert(Set(true).error==ERROR_ACCESS_DENIED && !registered && approvalWrites==prior);failRunWrite=false;
    loseRunWrite=true;assert(Set(true).error==ERROR_INVALID_DATA && !registered);loseRunWrite=false;
    failRead=true;assert(Read().status==Status::Unknown && Read().error==ERROR_ACCESS_DENIED);failRead=false;
    assert(Set(true).status==Status::Registered);kind=REG_BINARY;assert(Read().status==Status::InvalidPath);kind=REG_SZ;
    value.pop_back();assert(Read().status==Status::InvalidPath);
    puts("PASS: own startup toggle preserves Run; known approval/external changes/no-op, quotation/path repair, malformed/unknown refusal, write/delete/readback failures; mocked registry only");
}
