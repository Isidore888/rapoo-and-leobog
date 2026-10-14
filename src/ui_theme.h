#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace Ui {
constexpr COLORREF Background=RGB(36,50,71), Border=RGB(102,122,147), Text=RGB(243,249,255), Muted=RGB(205,223,236), Accent=RGB(164,222,242), Keyboard=RGB(205,189,235), Control=RGB(49,67,89);
inline int Scale(int n,int dpi){return MulDiv(n,dpi,96);}
inline HFONT Font(int size,int dpi,bool bold=false){return CreateFontW(-Scale(size,dpi),0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");}
inline void Box(HDC dc,RECT r,COLORREF fill,COLORREF border,int radius=8){
    HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,border);auto ob=SelectObject(dc,b);auto op=SelectObject(dc,p);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);DeleteObject(p);
}
inline void Surface(HDC dc,RECT r,int dpi){
    // Opaque gradient keeps text readable when the underlying desktop changes.
    for(int y=r.top;y<r.bottom;++y){int t=(y-r.top)*20/std::max(1L,r.bottom-r.top);HBRUSH b=CreateSolidBrush(RGB(36+t/5,50-t/2,71-t/3));RECT row{r.left,y,r.right,y+1};FillRect(dc,&row,b);DeleteObject(b);}
    HPEN p=CreatePen(PS_SOLID,1,Border);auto op=SelectObject(dc,p);auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));RoundRect(dc,r.left,r.top,r.right,r.bottom,Scale(22,dpi),Scale(22,dpi));SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(p);
}
inline void Label(HDC dc,const wchar_t* text,RECT r,int size,int dpi,COLORREF color=Text,UINT align=DT_LEFT,bool bold=false){
    HFONT f=Font(size,dpi,bold);auto old=SelectObject(dc,f);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,text,-1,&r,align|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);SelectObject(dc,old);DeleteObject(f);
}
inline void Connection(HDC dc,RECT r,int dpi,bool connected,bool wired){
    Box(dc,r,Control,Border,Scale(8,dpi));Label(dc,!connected?L"未连接":wired?L"USB":L"2.4G",r,12,dpi,Muted,DT_CENTER);
}
inline void DeviceIcon(HDC dc,int x,int y,int dpi,bool keyboard){
    auto S=[dpi](int n){return Scale(n,dpi);};HPEN p=CreatePen(PS_SOLID,std::max(1,S(1)),keyboard?Keyboard:Accent);auto op=SelectObject(dc,p);auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
    if(keyboard){RoundRect(dc,x,y+S(5),x+S(25),y+S(22),S(4),S(4));for(int row=0;row<2;++row)for(int col=0;col<4;++col){MoveToEx(dc,x+S(4+col*5),y+S(10+row*5),nullptr);LineTo(dc,x+S(6+col*5),y+S(10+row*5));}}
    else{RoundRect(dc,x+S(5),y,x+S(20),y+S(26),S(12),S(12));MoveToEx(dc,x+S(12),y+S(3),nullptr);LineTo(dc,x+S(12),y+S(10));}
    SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(p);
}
inline void ChargeIcon(HDC dc,int x,int y,int dpi){
    auto S=[dpi](int n){return Scale(n,dpi);};POINT points[]={{x+S(8),y},{x,y+S(10)},{x+S(5),y+S(10)},{x+S(2),y+S(18)},{x+S(12),y+S(7)},{x+S(7),y+S(7)}};
    HBRUSH b=CreateSolidBrush(Accent);auto ob=SelectObject(dc,b);auto op=SelectObject(dc,GetStockObject(NULL_PEN));Polygon(dc,points,6);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);
}
}
