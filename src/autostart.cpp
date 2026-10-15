#include "autostart.h"
#include <vector>
#include <array>

namespace Autostart {
static constexpr auto RunKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr auto ApprovalKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static constexpr auto Name=L"rapoo-tray";
std::wstring Command(const std::wstring& executable) {
    if(executable.empty() || executable.find(L'"')!=std::wstring::npos)return {};
    auto command=L"\""+executable+L"\"";
    // Run commands have a documented 260-character limit including arguments.
    return command.size()<=260?command:std::wstring{};
}
static std::wstring Executable() {
    std::vector<wchar_t> path(32768);
    DWORD size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!size || size>=path.size())return {};
    return std::wstring(path.data(),size);
}
static DWORD ReadValue(const wchar_t* key,DWORD& type,std::vector<BYTE>& data) {
    HKEY handle=nullptr;
    auto result=RegOpenKeyExW(HKEY_CURRENT_USER,key,0,KEY_QUERY_VALUE,&handle);
    if(result!=ERROR_SUCCESS)return result;
    DWORD size=0;result=RegQueryValueExW(handle,Name,nullptr,&type,nullptr,&size);
    if(result==ERROR_SUCCESS) {
        if(size>65536)result=ERROR_INVALID_DATA;
        else {data.resize(size);result=RegQueryValueExW(handle,Name,nullptr,&type,data.data(),&size);data.resize(size<=data.size()?size:0);}
    }
    RegCloseKey(handle);return result;
}
struct Approval { bool present=false,enabled=true,nativeFormat=true; DWORD error=ERROR_SUCCESS; };
static Approval ReadApproval() {
    Approval approval;DWORD type=0;std::vector<BYTE> data;
    auto result=ReadValue(ApprovalKey,type,data);
    if(result==ERROR_FILE_NOT_FOUND || result==ERROR_PATH_NOT_FOUND)return approval;
    if(result!=ERROR_SUCCESS){approval.error=result;return approval;}
    approval.present=true;
    // This machine's native Windows settings use 00 enabled / 01 disabled.
    // Retain Electron-compatible 02 enabled / 03 disabled as a separate pair.
    // StartupApproved is not a documented Windows API.
    // Validate length/type/reserved bytes and reject every other code instead of
    // treating arbitrary data as disabled or replacing unknown system records.
    if(type!=REG_BINARY || data.size()!=12 || data[1] || data[2] || data[3] ||
       data[0]>3) {
        approval.error=ERROR_NOT_SUPPORTED;return approval;
    }
    approval.nativeFormat=data[0]<2;
    approval.enabled=data[0]==0 || data[0]==2;return approval;
}
static State Failure(DWORD error) {auto state=Read();state.error=error;return state;}
State Read() {
    State state;DWORD type=0;std::vector<BYTE> data;
    auto result=ReadValue(RunKey,type,data);
    if(result==ERROR_FILE_NOT_FOUND || result==ERROR_PATH_NOT_FOUND){state.status=Status::Disabled;return state;}
    if(result!=ERROR_SUCCESS){state.error=result;return state;}
    if(type!=REG_SZ || data.size()<sizeof(wchar_t) || data.size()%sizeof(wchar_t)) {
        state.status=Status::InvalidPath;state.error=ERROR_INVALID_DATA;return state;
    }
    std::wstring value(data.size()/sizeof(wchar_t),L'\0');CopyMemory(value.data(),data.data(),data.size());
    if(value.back()!=0){state.status=Status::InvalidPath;state.error=ERROR_INVALID_DATA;return state;}
    value.pop_back();auto exe=Executable();auto command=Command(exe);
    if(exe.empty() || command.empty() || value.find(L'\0')!=std::wstring::npos ||
       (_wcsicmp(value.c_str(),command.c_str()) && _wcsicmp(value.c_str(),exe.c_str()))) {
        state.status=Status::InvalidPath;return state;
    }
    // A legacy unquoted path with spaces must be repaired before claiming it valid.
    if(!value.empty() && value.front()!=L'"' && exe.find_first_of(L" \t")!=std::wstring::npos){state.status=Status::InvalidPath;return state;}
    state.hasRegistration=true;
    auto approval=ReadApproval();state.approvalPresent=approval.present;
    if(approval.error){state.error=approval.error;return state;}
    state.status=approval.enabled?Status::Registered:Status::Disabled;
    return state;
}
State Set(bool enabled) {
    auto before=Read();
    if(before.status==Status::Unknown)return before;
    if(!enabled && !before.hasRegistration) {
        if(before.status==Status::Disabled)return before; // Never create a disabled orphan.
        before.error=ERROR_INVALID_DATA;return before;
    }
    auto approval=ReadApproval(); // Preflight before any registration repair/write.
    if(approval.error)return Failure(approval.error);
    HKEY key=nullptr;DWORD result=ERROR_SUCCESS;
    if(enabled && !before.hasRegistration) {
        auto command=Command(Executable());
        if(command.empty())return {Status::InvalidPath,ERROR_FILENAME_EXCED_RANGE,false};
        result=RegCreateKeyExW(HKEY_CURRENT_USER,RunKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
        if(result==ERROR_SUCCESS)result=::RegSetValueExW(key,Name,0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t)));
        if(key){RegCloseKey(key);key=nullptr;}
        if(result!=ERROR_SUCCESS)return Failure(result);
        auto registered=Read();
        if(!registered.hasRegistration || registered.error)return Failure(registered.error?registered.error:ERROR_INVALID_DATA);
    }
    // Recheck after a possible Run write. No deletion of the Run command; only
    // this application's approval changes when the user explicitly toggles it.
    approval=ReadApproval();
    if(approval.error)return Failure(approval.error);
    if(approval.enabled!=enabled) {
        if(enabled && !approval.nativeFormat) {
            result=RegOpenKeyExW(HKEY_CURRENT_USER,ApprovalKey,0,KEY_SET_VALUE,&key);
            if(result==ERROR_FILE_NOT_FOUND || result==ERROR_PATH_NOT_FOUND)result=ERROR_SUCCESS;
            else if(result==ERROR_SUCCESS) {
                result=RegDeleteValueW(key,Name);
                if(result==ERROR_FILE_NOT_FOUND)result=ERROR_SUCCESS;
            }
        } else {
            result=RegCreateKeyExW(HKEY_CURRENT_USER,ApprovalKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
            std::array<BYTE,12> record{};
            record[0]=enabled?0:(approval.nativeFormat?1:3);
            if(!enabled && approval.nativeFormat) {
                FILETIME disabledAt{};GetSystemTimeAsFileTime(&disabledAt);
                CopyMemory(record.data()+4,&disabledAt,sizeof(disabledAt));
            }
            if(result==ERROR_SUCCESS)result=::RegSetValueExW(key,Name,0,REG_BINARY,record.data(),DWORD(record.size()));
        }
    }
    if(key)RegCloseKey(key);
    if(result!=ERROR_SUCCESS)return Failure(result);
    auto state=Read();
    if(!state.hasRegistration || (enabled && state.status!=Status::Registered) || (!enabled && state.status!=Status::Disabled)) {
        if(!state.error)state.error=ERROR_INVALID_DATA;
    }
    return state;
}
const wchar_t* Label(const State& state) {
    switch(state.status) {
    case Status::Disabled:return L"未开启";
    case Status::Registered:return L"✓ 已开启";
    case Status::InvalidPath:return L"启动路径需修复";
    default:return L"状态未知";
    }
}
}
