#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
extern "C" {
#include <hidsdi.h>
}
#include <strsafe.h>
#include "rapoo_protocol.h"
#include "device_manager.h"
#include "osd_window.h"
#include "alert_window.h"
#include "tray_menu.h"
#include "keyboard_manager.h"
#include "serial_tasks.h"
#include "device_notifications.h"
#include "diagnostics.h"
#include <memory>

static const UINT WM_APP_TRAYMSG = WM_APP + 1;
static const UINT WM_APP_STATE_UPDATE = WM_APP + 2;

static HWND g_hMainWnd = NULL;
static NOTIFYICONDATAW g_nid = {0};
static UINT g_uTaskbarRestartMsg = 0;
static HICON g_hCurrentTrayIcon = NULL;
static ULONGLONG statusPopupUntil = 0;
static ULONGLONG keyboardFeedbackUntil = 0;
static bool hoverPending=false, hoverVisible=false;
static bool ignoreNextLeftUp=false;
static bool suppressStyleHover=false;
static constexpr UINT SettingCompletedMessage=WM_APP+7;
static SerialTasks mouseTasks,keyboardTasks;
static ULONGLONG settingSequence=0,latestSetting[5]{};
struct SettingCompletion {
    bool keyboard;int kind,value,result;ULONGLONG sequence,generation;int effect;
};
static bool StartSettingTasks(){if(!mouseTasks.Start())return false;if(keyboardTasks.Start())return true;mouseTasks.Stop();return false;}
static void StopSettingTasks(){mouseTasks.Stop();keyboardTasks.Stop();}
static void ScheduleMouseRefresh(bool request){mouseTasks.Submit(100,[request]{Device::RunControlRefresh(request);});}
static void PostSettingCompletion(HWND window,SettingCompletion completion) {
    auto payload=new SettingCompletion(completion);
    if(!PostMessageW(window,SettingCompletedMessage,0,reinterpret_cast<LPARAM>(payload)))delete payload;
}
static bool SubmitSetting(bool keyboard,int kind,int value) {
    auto mouse=Device::GetCurrentState();auto keys=Keyboard::GetCurrentState();
    if(keyboard ? !keys.isConnected || !keys.lightingWritable || !keys.lightingValid || !keys.sleepValid || keys.lightingStale || keys.sleepStale : !mouse.isConnected)return false;
    unsigned id=keyboard?2+kind:kind;
    if(id>=5)return false;
    auto sequence=++settingSequence;auto window=g_hMainWnd;
    bool submitted=(keyboard?keyboardTasks:mouseTasks).Submit(id,[=]{
        int result;
        if(keyboard) {
            result=int(kind==2?Keyboard::SetSleepTimeoutChecked(value,keys):Keyboard::SetLightingChecked(kind==1,value,keys));
        } else result=int(kind==1?Device::SetSleepTimeoutChecked(value,mouse.connectionGeneration):Device::SetPollingRateChecked(value,mouse.connectionGeneration));
        PostSettingCompletion(window,{keyboard,kind,value,result,sequence,
            keyboard?keys.connectionGeneration:mouse.connectionGeneration,keys.lightEffect});
    });
    if(submitted){latestSetting[id]=sequence;Tray::SetSettingPending(keyboard?3+kind:1+kind,true);}
    return submitted;
}
static void ShowKeyboardFeedback(const wchar_t* title,const wchar_t* detail,const wchar_t* model) {
    KillTimer(g_hMainWnd,3);KillTimer(g_hMainWnd,4);
    hoverPending=hoverVisible=false;statusPopupUntil=0;
    keyboardFeedbackUntil=GetTickCount64()+2500;
    // Confirmed captions have a parameter/value separator; errors keep their reason.
    const wchar_t* separator=wcschr(title,L'：');if(!separator)separator=wcschr(title,L':');
    if(model && separator && (!detail || !detail[0])) {
        std::wstring label(title,separator-title);const wchar_t* value=separator+1;while(*value==L' ')++value;
        Osd::ShowValue(label.c_str(),value,wcsstr(title,L"键盘")!=nullptr,2200);
    } else Osd::Show(title,detail,nullptr,2200);
}
static void ShowDeviceStatus() {
    auto mouse=Device::GetCurrentState();auto keyboard=Keyboard::GetCurrentState();
    POINT point{};GetCursorPos(&point);
    Osd::ShowStatus(mouse,keyboard,point,hoverVisible?0:4000);
}
static bool PointerAtTray() {
    NOTIFYICONIDENTIFIER id{sizeof(id)};id.hWnd=g_hMainWnd;id.uID=1;
    RECT rect{};POINT point{};GetCursorPos(&point);
    return SUCCEEDED(Shell_NotifyIconGetRect(&id,&rect)) && PtInRect(&rect,point);
}

static void RefreshTrayUI(const Device::State& state) {
    if (!g_hMainWnd) return;

    bool isDark = true;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD), type = 0;
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            isDark = (val == 0);
        }
        RegCloseKey(hKey);
    }

    int iconSize = GetSystemMetrics(SM_CXSMICON);
    if (iconSize <= 0) iconSize = 16;

    auto keyboard=Keyboard::GetCurrentState();
    Tray::UpdateMenuState(state,keyboard);
    HICON hNewIcon = Tray::CreateBatteryIcon(state.battery, state.isCharging, state.isConnected && state.batteryValid, iconSize, isDark,keyboard.battery,keyboard.isCharging,keyboard.isConnected && keyboard.batteryValid);
    if (hNewIcon) {
        g_nid.hIcon = hNewIcon;
        Tray::UpdateTooltip(g_nid, state,keyboard);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);

        if (g_hCurrentTrayIcon) {
            DestroyIcon(g_hCurrentTrayIcon);
        }
        g_hCurrentTrayIcon = hNewIcon;
    }

    Alert::CheckBattery(state,keyboard);
    Osd::UpdateStatus(state,keyboard);
}

static void OnDeviceStateChanged(const Device::State&, DWORD changeMask) {
    // All window operations run on the main thread.
    if(g_hMainWnd) PostMessageW(g_hMainWnd,WM_APP_STATE_UPDATE,changeMask,1);
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestartMsg) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        RefreshTrayUI(Device::GetCurrentState());
        return 0;
    }

    switch (msg) {
        case WM_APP_TRAYMSG: {
            if (lParam == WM_LBUTTONUP) {
                if(ignoreNextLeftUp){ignoreNextLeftUp=false;return 0;}
                suppressStyleHover=false;
                hoverVisible=false;hoverPending=false;KillTimer(hWnd,4);
                SetTimer(hWnd,3,GetDoubleClickTime(),nullptr);
            } else if (lParam == WM_RBUTTONUP) {
                KillTimer(hWnd,3);KillTimer(hWnd,4);hoverPending=hoverVisible=false;statusPopupUntil=0;Osd::HideStatus();
                SetForegroundWindow(hWnd);
                Tray::ShowMenu(hWnd);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                ignoreNextLeftUp=true;
                KillTimer(hWnd,3);KillTimer(hWnd,4);hoverPending=hoverVisible=false;statusPopupUntil=0;Osd::HideStatus();
                Tray::CycleBatteryStyle();
                suppressStyleHover=true;
                RefreshTrayUI(Device::GetCurrentState());
            } else if(lParam==WM_MOUSEMOVE && !suppressStyleHover && !hoverPending && !hoverVisible && GetTickCount64()>=statusPopupUntil && GetTickCount64()>=keyboardFeedbackUntil) {
                hoverPending=true;SetTimer(hWnd,4,500,nullptr);
            }
            return 0;
        }

        case WM_APP_STATE_UPDATE: {
            auto mouse=Device::GetCurrentState();
            RefreshTrayUI(mouse);
            if(GetTickCount64()>=statusPopupUntil && GetTickCount64()>=keyboardFeedbackUntil && !hoverVisible && lParam==1 && mouse.isConnected && mouse.batteryValid && (wParam & Device::CHANGE_DPI))
                Osd::ShowDpiUpdate(mouse.dpiLevel,mouse.dpiX,mouse.dpiY,mouse.battery,mouse.pollingHz,mouse.isCharging,mouse.isWired,mouse.modelName);
            return 0;
        }

        case Tray::KeyboardSettingMessage: {
            auto setting=static_cast<KeyboardProtocol::Setting>(wParam);
            if(setting!=KeyboardProtocol::Setting::Brightness && setting!=KeyboardProtocol::Setting::Speed && setting!=KeyboardProtocol::Setting::Sleep)return 0;
            KillTimer(hWnd,3);KillTimer(hWnd,4);
            hoverPending=hoverVisible=false;statusPopupUntil=0;
            int kind=setting==KeyboardProtocol::Setting::Sleep?2:setting==KeyboardProtocol::Setting::Speed?1:0;
            if(!SubmitSetting(true,kind,static_cast<int>(lParam)))ShowKeyboardFeedback(L"键盘设置未发送",L"配置不可用或任务队列已关闭",nullptr);
            return 0;
        }
        case Tray::MouseSettingMessage: {
            if(wParam>1)return 0;
            if(!SubmitSetting(false,int(wParam),int(lParam)))ShowKeyboardFeedback(L"鼠标设置未发送",L"鼠标未连接或任务队列已关闭",nullptr);
            return 0;
        }
        case SettingCompletedMessage: {
            std::unique_ptr<SettingCompletion> completion(reinterpret_cast<SettingCompletion*>(lParam));
            if(!completion)return 0;
            const auto& result=*completion;unsigned id=result.keyboard?2+result.kind:result.kind;
            if(id>=5 || latestSetting[id]!=result.sequence)return 0;
            Tray::SetSettingPending(result.keyboard?3+result.kind:1+result.kind,false);
            auto mouse=Device::GetCurrentState();auto keys=Keyboard::GetCurrentState();
            Tray::UpdateMenuState(mouse,keys);
            if(result.keyboard ? !keys.isConnected || keys.connectionGeneration!=result.generation || keys.lightEffect!=result.effect :
                !mouse.isConnected || mouse.connectionGeneration!=result.generation)return 0;
            RefreshTrayUI(mouse);
            wchar_t title[96],detail[96];
            if(result.keyboard) {
                auto outcome=static_cast<Keyboard::LightingResult>(result.result);
                bool confirmed=outcome==Keyboard::LightingResult::Confirmed;
                if(confirmed && (result.kind==2? !keys.sleepValid || keys.sleepSeconds!=result.value :
                    !keys.lightingValid || (result.kind==1?keys.lightSpeed:keys.brightness)!=result.value))return 0;
                auto setting=result.kind==2?KeyboardProtocol::Setting::Sleep:result.kind==1?KeyboardProtocol::Setting::Speed:KeyboardProtocol::Setting::Brightness;
                Keyboard::FormatSettingPopup(setting,result.value,outcome,title,96,detail,96);
                ShowKeyboardFeedback(title,detail,confirmed?keys.modelName:nullptr);
            } else {
                auto outcome=static_cast<Device::SettingResult>(result.result);
                bool confirmed=outcome==Device::SettingResult::Confirmed;
                if(confirmed && (result.kind==1?mouse.sleepMinutes:mouse.pollingHz)!=result.value)return 0;
                StringCchPrintfW(title,96,result.kind==1?(confirmed?L"休眠时间: %d 分钟":L"尝试设置休眠: %d 分钟"):
                    (confirmed?L"回报率: %d Hz":L"尝试设置回报率: %d Hz"),result.value);
                StringCchCopyW(detail,96,Device::SettingResultText(outcome));
                ShowKeyboardFeedback(title,detail,confirmed?mouse.modelName:nullptr);
            }
            return 0;
        }

        case Keyboard::SettingsChangedMessage: {
            auto keyboard=Keyboard::GetCurrentState();
            DWORD changes=DWORD(wParam)&(Keyboard::BrightnessChanged|Keyboard::SpeedChanged);
            if(!changes || !keyboard.isConnected || !keyboard.lightingValid || keyboard.lightingRevision!=static_cast<ULONGLONG>(lParam))return 0;
            wchar_t title[96],detail[96]{};
            if(changes==(Keyboard::BrightnessChanged|Keyboard::SpeedChanged)) {
                StringCchCopyW(title,96,L"键盘灯光设置");
                StringCchPrintfW(detail,96,L"亮度 %d%% · 速度 %d%%",keyboard.brightness*25,keyboard.lightSpeed*25);
            } else {
                Keyboard::FormatSettingPopup(changes&Keyboard::BrightnessChanged?KeyboardProtocol::Setting::Brightness:KeyboardProtocol::Setting::Speed,
                    changes&Keyboard::BrightnessChanged?keyboard.brightness:keyboard.lightSpeed,Keyboard::LightingResult::Confirmed,title,96,detail,96);
            }
            ShowKeyboardFeedback(title,detail,keyboard.modelName);
            return 0;
        }

        case WM_APP + 3:
            Alert::ShowSettings(hWnd); return 0;
        case WM_TIMER:
            if(wParam==1) RefreshTrayUI(Device::GetCurrentState());
            else if(wParam==3) {
                KillTimer(hWnd,3);Device::ForceRefresh();Keyboard::ForceRefresh();statusPopupUntil=GetTickCount64()+4000;
                ShowDeviceStatus();RefreshTrayUI(Device::GetCurrentState());
            } else if(wParam==4) {
                KillTimer(hWnd,4);hoverPending=false;
                if(GetTickCount64()>=keyboardFeedbackUntil && PointerAtTray()) {hoverVisible=true;ShowDeviceStatus();}
            } else if(wParam==2 && suppressStyleHover && !PointerAtTray())suppressStyleHover=false;
            else if(wParam==2 && hoverVisible && !PointerAtTray()) {
                RECT rect{};POINT point{};GetCursorPos(&point);GetWindowRect(Osd::GetWindowHandle(),&rect);
                if(!PtInRect(&rect,point)){hoverVisible=false;Osd::HideStatus();}
            }
            return 0;
        case WM_POWERBROADCAST:
            if(wParam==PBT_APMRESUMEAUTOMATIC || wParam==PBT_APMRESUMESUSPEND){Device::NotifyDeviceChange();Keyboard::NotifyDeviceChange();}
            return TRUE;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            RefreshTrayUI(Device::GetCurrentState());
            return 0;
        }

        case WM_DEVICECHANGE: {
            const auto targets=DeviceNotifications::Targets(wParam,lParam);
            if(targets)Diagnostics::Log("pnp_interface",0,0,0,targets,0,wParam);
            if(targets & DeviceNotifications::Mouse)Device::NotifyDeviceChange();
            if(targets & DeviceNotifications::Keyboard)Keyboard::NotifyDeviceChange();
            return 0;
        }

        case WM_CLOSE:
            Diagnostics::Log("shutdown_begin");
            Device::BeginShutdown();Keyboard::BeginShutdown();
            StopSettingTasks();
            DestroyWindow(hWnd);return 0;
        case WM_DESTROY: {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hCurrentTrayIcon) {
                DestroyIcon(g_hCurrentTrayIcon);
                g_hCurrentTrayIcon = NULL;
            }
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Single instance protection
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\rapoo-traySingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    Tray::InitTheme();

    // Enable modern Per-Monitor V2 DPI awareness
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *pfnSetDpiAwareV2)(DPI_AWARENESS_CONTEXT);
        pfnSetDpiAwareV2 fnSetDpiAwareV2 = (pfnSetDpiAwareV2)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (fnSetDpiAwareV2) {
            fnSetDpiAwareV2(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"RapooTrayMessageWnd";
    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(
        0, wc.lpszClassName, L"RapooTray",
        WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    g_uTaskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Initialize OSD and Alert Windows
    Osd::Init(hInstance);
    Alert::Init(hInstance);
    if(!Diagnostics::Init())MessageBoxW(g_hMainWnd,L"诊断日志无法创建，请检查程序目录写入权限。",L"诊断日志",MB_OK|MB_ICONWARNING);

    // Register PnP Device notifications
    DEV_BROADCAST_DEVICEINTERFACE_W dbFilter = {0};
    dbFilter.dbcc_size = sizeof(dbFilter);
    dbFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&dbFilter.dbcc_classguid);
    HDEVNOTIFY hDevNotify = RegisterDeviceNotificationW(g_hMainWnd, &dbFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Register Notify Icon
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON;
    g_nid.uCallbackMessage = WM_APP_TRAYMSG;
    g_nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"鼠标与键盘 (正在连接...)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Queue control reads before starting input reception.
    bool queuesStarted=StartSettingTasks();Device::SetControlScheduler(ScheduleMouseRefresh);
    // Start background HID Device Manager
    if(!queuesStarted || !Device::Start(g_hMainWnd, OnDeviceStateChanged) || !Keyboard::Start(g_hMainWnd,WM_APP_STATE_UPDATE)) {
        MessageBoxW(g_hMainWnd,L"无法启动设备读取线程，请重新启动程序。",L"启动失败",MB_OK|MB_ICONERROR);
        DestroyWindow(g_hMainWnd);
    }
    SetTimer(g_hMainWnd,1,10000,nullptr);
    SetTimer(g_hMainWnd,2,100,nullptr);

    // Run message pump
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if(Alert::HandleSettingsMessage(&msg))continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Clean exit
    if (hDevNotify) {
        UnregisterDeviceNotification(hDevNotify);
        hDevNotify = NULL;
    }
    Device::BeginShutdown();Keyboard::BeginShutdown();
    StopSettingTasks();
    // Results posted while the window was closing must not leak heap payloads.
    MSG completion;
    while(PeekMessageW(&completion,nullptr,SettingCompletedMessage,SettingCompletedMessage,PM_REMOVE))
        delete reinterpret_cast<SettingCompletion*>(completion.lParam);
    Keyboard::Stop();
    Device::Stop();
    Device::SetControlScheduler(nullptr);
    Osd::Cleanup();
    Alert::Cleanup();
    Diagnostics::Close();

    CloseHandle(hMutex);
    return 0;
}
