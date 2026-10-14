#include "alert_window.h"
#include <strsafe.h>
#include <cwchar>
#include <string>
#include <dwmapi.h>
#include "ui_theme.h"
namespace Alert {
static HWND settingsWindow=nullptr,settingsNotifyWindow=nullptr;
static HFONT settingsFont=nullptr;
static HBRUSH fieldBrush=nullptr;
static WNDPROC enableProc=nullptr;
static int dpi=96,errorField=0;
static std::wstring settingsError;
static constexpr DWORD PageStyle=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
static constexpr DWORD PageExStyle=WS_EX_CONTROLPARENT|WS_EX_APPWINDOW;
static int Scale(int n){return MulDiv(n,dpi,96);}
static int WindowDpi(HWND h) {
    auto get=(UINT(WINAPI*)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow");
    int value=get?get(h):96;return value>0?value:96;
}
static HWND Control(HWND parent,const wchar_t* cls,const wchar_t* text,DWORD style,int id) {
    HWND child=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(child,WM_SETFONT,(WPARAM)settingsFont,TRUE);return child;
}
static void EnableFields(HWND h,int device) {
    bool enabled=SendDlgItemMessageW(h,200+device*10,BM_GETCHECK,0,0)==BST_CHECKED;
    for(int j=1;j<=2;++j)EnableWindow(GetDlgItem(h,200+device*10+j),enabled);
    InvalidateRect(h,nullptr,FALSE);
}
static void LayoutSettings(HWND h) {
    for(int i=0;i<2;++i) {
        const int y=138+i*76,base=200+i*10;
        for(int j=1;j<=2;++j) {
            HWND edit=GetDlgItem(h,base+j);int x=j==1?178:324;
            MoveWindow(edit,Scale(x+2),Scale(y+2),Scale(84),Scale(36),TRUE);
            HDC dc=GetDC(edit);auto old=SelectObject(dc,settingsFont);TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);SelectObject(dc,old);ReleaseDC(edit,dc);
            // Native formatting adds vertical padding without replacing text editing.
            RECT r{Scale(6),(Scale(36)-tm.tmHeight)/2,Scale(78),(Scale(36)+tm.tmHeight)/2};
            SendMessageW(edit,EM_SETRECTNP,0,(LPARAM)&r);
        }
        MoveWindow(GetDlgItem(h,base),Scale(470),Scale(y),Scale(56),Scale(40),TRUE);
        EnableFields(h,i);
    }
    MoveWindow(GetDlgItem(h,IDCANCEL),Scale(350),Scale(310),Scale(80),Scale(36),TRUE);
    MoveWindow(GetDlgItem(h,IDOK),Scale(446),Scale(310),Scale(90),Scale(36),TRUE);
}
static RECT PageRect() {
    RECT r{0,0,Scale(560),Scale(360)};
    auto adjust=(BOOL(WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT))GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi");
    if(adjust)adjust(&r,PageStyle,FALSE,PageExStyle,dpi);else AdjustWindowRectEx(&r,PageStyle,FALSE,PageExStyle);
    return r;
}
static LRESULT CALLBACK EnableProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT || msg==WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};HDC dc=msg==WM_PAINT?BeginPaint(h,&ps):(HDC)wp;RECT r;GetClientRect(h,&r);
        HBRUSH b=CreateSolidBrush(Ui::Background);FillRect(dc,&r,b);DeleteObject(b);
        bool on=SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED;
        COLORREF accent=GetDlgCtrlID(h)==210?Ui::Keyboard:Ui::Accent;
        Ui::Label(dc,on?L"开":L"关",RECT{0,0,Scale(18),r.bottom},12,dpi,on?Ui::Text:Ui::Muted,DT_CENTER);
        RECT toggle{Scale(20),Scale(10),Scale(56),Scale(30)};
        Ui::Box(dc,toggle,on?accent:Ui::Border,on?accent:Ui::Border,Scale(20));
        int x=toggle.left+Scale(on?20:3);Ui::Box(dc,RECT{x,toggle.top+Scale(3),x+Scale(13),toggle.top+Scale(16)},Ui::Background,Ui::Background,Scale(13));
        if(GetFocus()==h && !(SendMessageW(h,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))DrawFocusRect(dc,&r);
        if(msg==WM_PAINT)EndPaint(h,&ps);return 0;
    }
    auto result=CallWindowProcW(enableProc,h,msg,wp,lp);
    if(msg==BM_SETCHECK || msg==WM_SETFOCUS || msg==WM_KILLFOCUS)InvalidateRect(h,nullptr,FALSE);
    return result;
}
static bool ReadThreshold(HWND h,int id,int& value) {
    wchar_t text[4];int length=GetWindowTextLengthW(GetDlgItem(h,id));
    if(length<1 || length>3)return false;
    GetDlgItemTextW(h,id,text,4);value=0;
    for(int i=0;i<length;++i){if(text[i]<L'0' || text[i]>L'9')return false;value=value*10+text[i]-L'0';}
    return value>=1 && value<=100;
}
static void Divider(HDC dc,int y) {
    HPEN p=CreatePen(PS_SOLID,1,Ui::Border);auto old=SelectObject(dc,p);
    MoveToEx(dc,Scale(24),Scale(y),nullptr);LineTo(dc,Scale(536),Scale(y));SelectObject(dc,old);DeleteObject(p);
}
static LRESULT CALLBACK SettingsProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_CREATE) {
        dpi=WindowDpi(h);
        settingsFont=Ui::Font(16,dpi);fieldBrush=CreateSolidBrush(Ui::Control);settingsError.clear();errorField=0;
        for(int i=0;i<2;++i) {
            int base=200+i*10;auto c=GetConfig(i);
            for(int j=1;j<=2;++j){wchar_t value[8];StringCchPrintfW(value,8,L"%d",j==1?c.warning:c.critical);
                HWND edit=Control(h,L"EDIT",value,ES_NUMBER|ES_CENTER|ES_MULTILINE|WS_TABSTOP,base+j);SendMessageW(edit,EM_SETLIMITTEXT,3,0);}
            HWND enable=Control(h,L"BUTTON",i?L"键盘提醒":L"鼠标提醒",BS_AUTOCHECKBOX|WS_TABSTOP,base);
            auto old=(WNDPROC)SetWindowLongPtrW(enable,GWLP_WNDPROC,(LONG_PTR)EnableProc);if(!enableProc)enableProc=old;
            SendMessageW(enable,BM_SETCHECK,c.enabled?BST_CHECKED:BST_UNCHECKED,0);
        }
        Control(h,L"BUTTON",L"取消",BS_OWNERDRAW|WS_TABSTOP,IDCANCEL);
        Control(h,L"BUTTON",L"保存",BS_OWNERDRAW|WS_TABSTOP,IDOK);
        LayoutSettings(h);SetFocus(GetDlgItem(h,IsWindowEnabled(GetDlgItem(h,201))?201:200));return 0;
    }
    if(msg==WM_COMMAND) {
        int id=LOWORD(wp),event=HIWORD(wp);
        if(event==EN_CHANGE){settingsError.clear();errorField=0;InvalidateRect(h,nullptr,FALSE);}
        if(event==EN_SETFOCUS || event==EN_KILLFOCUS)InvalidateRect(h,nullptr,FALSE);
        if((id==200 || id==210) && event==BN_CLICKED){EnableFields(h,(id-200)/10);return 0;}
        if(id==IDCANCEL){DestroyWindow(h);return 0;}
        if(id==IDOK) {
            AlertPolicy::Config c[2];
            for(int i=0;i<2;++i) {
                int base=200+i*10;c[i].enabled=SendDlgItemMessageW(h,base,BM_GETCHECK,0,0)==BST_CHECKED;
                bool warning=ReadThreshold(h,base+1,c[i].warning),critical=ReadThreshold(h,base+2,c[i].critical);
                if(!warning || !critical || !AlertPolicy::Valid(c[i])) {
                    errorField=!warning?base+1:base+2;settingsError=i?L"键盘：":L"鼠标：";
                    settingsError+=!warning || !critical?L"请输入 1–100 的整数。":L"严重阈值须小于低电量阈值。";
                    SetFocus(GetDlgItem(h,c[i].enabled?errorField:base));InvalidateRect(h,nullptr,FALSE);return 0;
                }
            }
            if(!SaveConfig(c)){settingsError=L"保存失败：无法写入当前用户配置。";InvalidateRect(h,nullptr,FALSE);return 0;}
            if(IsWindow(settingsNotifyWindow))PostMessageW(settingsNotifyWindow,WM_APP+2,0,0);
            DestroyWindow(h);return 0;
        }
    }
    if(msg==WM_PAINT || msg==WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};HDC dc=msg==WM_PAINT?BeginPaint(h,&ps):(HDC)wp;RECT r;GetClientRect(h,&r);
        HBRUSH b=CreateSolidBrush(Ui::Background);FillRect(dc,&r,b);DeleteObject(b);
        Ui::Label(dc,L"低电量提醒",RECT{Scale(24),Scale(20),Scale(536),Scale(54)},22,dpi,Ui::Text,DT_LEFT,true);
        Ui::Label(dc,L"设备",RECT{Scale(24),Scale(78),Scale(148),Scale(106)},13,dpi,Ui::Muted);
        Ui::Label(dc,L"低电量 (%)",RECT{Scale(170),Scale(78),Scale(274),Scale(106)},13,dpi,Ui::Muted,DT_CENTER);
        Ui::Label(dc,L"严重低电量 (%)",RECT{Scale(300),Scale(78),Scale(436),Scale(106)},13,dpi,Ui::Muted,DT_CENTER);
        Ui::Label(dc,L"启用",RECT{Scale(462),Scale(78),Scale(536),Scale(106)},13,dpi,Ui::Muted,DT_CENTER);
        Divider(dc,114);Divider(dc,192);Divider(dc,278);
        for(int i=0;i<2;++i) {
            int base=200+i*10,y=138+i*76;
            Ui::DeviceIcon(dc,Scale(26),Scale(y+7),dpi,i==1);
            Ui::Label(dc,i?L"键盘":L"鼠标",RECT{Scale(64),Scale(y),Scale(148),Scale(y+40)},16,dpi,Ui::Text,DT_LEFT,true);
            for(int j=1;j<=2;++j){int x=j==1?178:324,id=base+j;HWND edit=GetDlgItem(h,id);
                COLORREF border=id==errorField?RGB(255,184,138):GetFocus()==edit?Ui::Accent:Ui::Border;
                Ui::Box(dc,RECT{Scale(x),Scale(y),Scale(x+88),Scale(y+40)},Ui::Control,border,Scale(10));}
        }
        if(!settingsError.empty())Ui::Label(dc,settingsError.c_str(),RECT{Scale(24),Scale(283),Scale(536),Scale(306)},12,dpi,RGB(255,184,138));
        if(msg==WM_PAINT)EndPaint(h,&ps);return 0;
    }
    if(msg==WM_DRAWITEM) {
        auto item=(DRAWITEMSTRUCT*)lp;bool save=item->CtlID==IDOK,pressed=item->itemState&ODS_SELECTED;
        Ui::Box(item->hDC,item->rcItem,save?(pressed?Ui::Muted:Ui::Accent):pressed?Ui::Control:Ui::Background,save?Ui::Accent:Ui::Border,Scale(10));
        wchar_t label[16];GetWindowTextW(item->hwndItem,label,16);Ui::Label(item->hDC,label,item->rcItem,14,dpi,save?Ui::Background:Ui::Text,DT_CENTER,save);
        if(item->itemState&ODS_FOCUS){RECT r=item->rcItem;InflateRect(&r,-Scale(3),-Scale(3));DrawFocusRect(item->hDC,&r);}return TRUE;
    }
    if(msg==WM_CTLCOLORSTATIC || msg==WM_CTLCOLOREDIT || msg==WM_CTLCOLORBTN) {
        HDC dc=(HDC)wp;HWND child=(HWND)lp;int id=GetDlgCtrlID(child);bool edit=id==201 || id==202 || id==211 || id==212;
        SetTextColor(dc,IsWindowEnabled(child)?Ui::Text:Ui::Border);SetBkColor(dc,edit?Ui::Control:Ui::Background);
        return (LRESULT)(edit?fieldBrush:GetStockObject(NULL_BRUSH));
    }
    if(msg==WM_DPICHANGED) {
        dpi=HIWORD(wp);HFONT old=settingsFont;settingsFont=Ui::Font(16,dpi);
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,(WPARAM)settingsFont,TRUE);
        DeleteObject(old);auto suggested=(RECT*)lp;RECT r=PageRect();
        SetWindowPos(h,nullptr,suggested->left,suggested->top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
        LayoutSettings(h);InvalidateRect(h,nullptr,FALSE);return 0;
    }
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_CLOSE){DestroyWindow(h);return 0;}
    if(msg==WM_DESTROY){settingsWindow=settingsNotifyWindow=nullptr;DeleteObject(settingsFont);settingsFont=nullptr;DeleteObject(fieldBrush);fieldBrush=nullptr;return 0;}
    return DefWindowProcW(h,msg,wp,lp);
}
void CloseSettings(){if(settingsWindow)DestroyWindow(settingsWindow);}
void ShowSettings(HWND owner) {
    if(settingsWindow){ShowWindow(settingsWindow,SW_RESTORE);SetForegroundWindow(settingsWindow);return;}
    settingsNotifyWindow=owner;
    dpi=WindowDpi(owner);
    WNDCLASSW wc{};wc.lpfnWndProc=SettingsProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(1));wc.lpszClassName=L"DualBatteryAlertSettings";RegisterClassW(&wc);
    POINT cursor;GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);
    RECT r=PageRect();
    settingsWindow=CreateWindowExW(PageExStyle,wc.lpszClassName,L"低电量提醒设置",PageStyle,(mi.rcWork.left+mi.rcWork.right-(r.right-r.left))/2,(mi.rcWork.top+mi.rcWork.bottom-(r.bottom-r.top))/2,r.right-r.left,r.bottom-r.top,nullptr,nullptr,wc.hInstance,nullptr);
    BOOL dark=TRUE;DwmSetWindowAttribute(settingsWindow,20,&dark,sizeof(dark));
    ShowWindow(settingsWindow,SW_SHOW);
    // Use the page's actual monitor DPI; an invisible owner can belong to a different monitor.
    r=PageRect();
    // SHOWWINDOW also overrides a launcher's initial hidden startup state.
    SetWindowPos(settingsWindow,nullptr,(mi.rcWork.left+mi.rcWork.right-(r.right-r.left))/2,(mi.rcWork.top+mi.rcWork.bottom-(r.bottom-r.top))/2,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_SHOWWINDOW);
    SetForegroundWindow(settingsWindow);
}
bool HandleSettingsMessage(MSG* msg) {
    if(!settingsWindow)return false;
    if(msg->message==WM_KEYDOWN && (msg->hwnd==settingsWindow || IsChild(settingsWindow,msg->hwnd))) {
        int id=GetDlgCtrlID(msg->hwnd);
        if(msg->wParam==VK_RETURN){if(id==200 || id==210)SendMessageW(msg->hwnd,BM_CLICK,0,0);else SendMessageW(settingsWindow,WM_COMMAND,id==IDCANCEL?IDCANCEL:IDOK,0);return true;}
        if(msg->wParam==VK_ESCAPE){SendMessageW(settingsWindow,WM_COMMAND,IDCANCEL,0);return true;}
    }
    return IsDialogMessageW(settingsWindow,msg);
}
}
