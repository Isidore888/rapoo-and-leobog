// Opt-in own startup validation. No HID/UI initialization and no other value names.
// --read-own-run is read only; --verify-own-toggle requires explicit authorization
// to change the own StartupApproved value and leaves the valid Run command enabled.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
static std::wstring trayPath;
static DWORD WINAPI InstalledPath(HMODULE,wchar_t* output,DWORD size) {
    if(trayPath.size()+1>size)return size;
    std::copy(trayPath.begin(),trayPath.end(),output);output[trayPath.size()]=0;
    return DWORD(trayPath.size());
}
#define GetModuleFileNameW InstalledPath
#include "../src/autostart.cpp"
#undef GetModuleFileNameW
struct RunValue {DWORD result=ERROR_SUCCESS,type=0;std::vector<BYTE> bytes;
    bool operator==(const RunValue& other)const{return result==other.result && type==other.type && bytes==other.bytes;}};
static RunValue OwnRun() {
    RunValue result;HKEY key=nullptr;
    result.result=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key);
    if(result.result!=ERROR_SUCCESS)return result;
    DWORD size=0;result.result=RegQueryValueExW(key,L"rapoo-tray",nullptr,&result.type,nullptr,&size);
    if(result.result==ERROR_SUCCESS) {
        if(size>65536)result.result=ERROR_INVALID_DATA;
        else {result.bytes.resize(size);result.result=RegQueryValueExW(key,L"rapoo-tray",nullptr,&result.type,result.bytes.data(),&size);}
    }
    RegCloseKey(key);return result;
}
static void PrintApproval(const char* phase) {
    DWORD type=0;std::vector<BYTE> bytes;
    auto result=Autostart::ReadValue(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",type,bytes);
    printf("approval phase=%s result=%lu type=%lu bytes=",phase,result,type);
    for(auto byte:bytes)printf("%02X ",byte);
    puts("");fflush(stdout);
}
int main(int argc,char** argv) {
    const bool visible=argc==2 && !strcmp(argv[1],"--verify-own-toggle-visible");
    if(argc!=2 || (!visible && strcmp(argv[1],"--read-own-run") && strcmp(argv[1],"--verify-own-toggle")))return 2;
    wchar_t path[32768];auto length=GetFullPathNameW(L"bin\\rapoo-tray.exe",32768,path,nullptr);
    if(!length || length>=32768 || GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES)return 3;
    trayPath.assign(path,length);
    auto original=Autostart::Read();
    printf("initial_status=%d valid_run=%d approval_present=%d error=%lu\n",int(original.status),original.hasRegistration,original.approvalPresent,original.error);
    PrintApproval("initial");
    if(!strcmp(argv[1],"--read-own-run"))return original.error?4:0;
    if(original.status!=Autostart::Status::Registered || original.error || !original.hasRegistration) {
        puts("STOP: original own registration is not valid/enabled; no writes");return 4;
    }
    const auto baseline=OwnRun();
    if(baseline.result!=ERROR_SUCCESS){puts("STOP: cannot snapshot own Run; no writes");return 4;}
    auto disabled=Autostart::Set(false);const auto disabledRun=OwnRun();
    const bool disableOk=disabled.status==Autostart::Status::Disabled && disabled.hasRegistration && !disabled.error && disabledRun==baseline;
    PrintApproval("disabled");
    if(visible && disableOk) {
        puts("disabled_ready=1 restore_after_seconds=60");fflush(stdout);
        const auto deadline=GetTickCount64()+60000;
        while(GetTickCount64()<deadline && GetFileAttributesW(L"verification\\startup-sync-and-active\\resume-20261012\\restore-startup.signal")==INVALID_FILE_ATTRIBUTES)Sleep(200);
    }
    // One authorized restoration to enabled, only through validated production logic.
    // An unknown/unreadable system format is never overridden to force restoration.
    auto enabled=Autostart::Set(true);const auto finalRun=OwnRun();
    auto final=Autostart::Read();
    PrintApproval("final");
    bool enableOk=enabled.status==Autostart::Status::Registered && !enabled.error && final.status==Autostart::Status::Registered && !final.error;
    printf("disabled_readback=%d run_retained_while_disabled=%d reenabled_readback=%d final_run_unchanged=%d error_disable=%lu error_enable=%lu\n",
        disableOk,disabledRun==baseline,enableOk,finalRun==baseline,disabled.error,enabled.error);
    puts("scope=HKCU_Run_and_StartupApproved_rapoo-tray_only other_names_written=0 run_deleted=0 device_io=0 logon_test=0 final_intent=enabled");
    return disableOk && enableOk && finalRun==baseline?0:5;
}
