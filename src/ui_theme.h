#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Ui {
#ifndef GetRValue
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb) >> 16))
#endif
constexpr COLORREF Background=RGB(36,50,71), Border=RGB(102,122,147), Text=RGB(243,249,255), Muted=RGB(205,223,236), Accent=RGB(164,222,242), Keyboard=RGB(205,189,235), Control=RGB(49,67,89);
inline int Scale(int n,int dpi){return MulDiv(n,dpi,96);}
inline HFONT Font(int size,int dpi,bool bold=false){return CreateFontW(-Scale(size,dpi),0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");}

// Apply Windows 11 hardware-antialiased rounded corners and native drop shadow via DWM,
// falling back to SetWindowRgn only on older systems where DWMWA_WINDOW_CORNER_PREFERENCE fails.
inline void ApplyRoundCorners(HWND hWnd, int width, int height, int radius, COLORREF borderColor = Border) {
    if (!hWnd) return;
    DWORD corner = 2; // DWMWCP_ROUND
    HRESULT hr = DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
    if (SUCCEEDED(hr)) {
        DwmSetWindowAttribute(hWnd, 34, &borderColor, sizeof(borderColor));
        SetWindowRgn(hWnd, nullptr, TRUE);
    } else if (width > 0 && height > 0) {
        HRGN rgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
        SetWindowRgn(hWnd, rgn, TRUE);
    }
}

// Lightweight 32-bit DIB canvas for subpixel Signed Distance Field (SDF) anti-aliased rendering.
// Works on any target HDC without external GDI+ dependencies or persistent GDI handles.
class AaCanvas {
    HDC targetDc = nullptr;
    HDC memDc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    RECT bounds{};
    int w = 0, h = 0;
    uint32_t* pixels = nullptr;
public:
    AaCanvas(HDC dc, RECT r, bool copyBackground = true) : targetDc(dc), bounds(r) {
        w = int(r.right - r.left);
        h = int(r.bottom - r.top);
        if (!dc || w <= 0 || h <= 0) return;
        memDc = CreateCompatibleDC(dc);
        if (!memDc) return;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            DeleteDC(memDc);
            dib = nullptr;
            memDc = nullptr;
            return;
        }
        pixels = static_cast<uint32_t*>(bits);
        oldBmp = SelectObject(memDc, dib);
        if (copyBackground) {
            BitBlt(memDc, 0, 0, w, h, dc, r.left, r.top, SRCCOPY);
        }
    }
    ~AaCanvas() {
        if (memDc && dib) {
            BitBlt(targetDc, bounds.left, bounds.top, w, h, memDc, 0, 0, SRCCOPY);
            SelectObject(memDc, oldBmp);
            DeleteObject(dib);
            DeleteDC(memDc);
        }
    }
    bool Valid() const { return pixels != nullptr; }
    int Width() const { return w; }
    int Height() const { return h; }
    void BlendPixel(int x, int y, COLORREF color, float alpha) {
        if (x < 0 || x >= w || y < 0 || y >= h || alpha <= 0.0f) return;
        const uint8_t sr = GetRValue(color), sg = GetGValue(color), sb = GetBValue(color);
        uint32_t& dst = pixels[y * w + x];
        if (alpha >= 1.0f) {
            dst = (0xFFu << 24) | (uint32_t(sr) << 16) | (uint32_t(sg) << 8) | uint32_t(sb);
            return;
        }
        const uint8_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
        const uint8_t rr = uint8_t(dr + (sr - dr) * alpha + 0.5f);
        const uint8_t rg = uint8_t(dg + (sg - dg) * alpha + 0.5f);
        const uint8_t rb = uint8_t(db + (sb - db) * alpha + 0.5f);
        dst = (0xFFu << 24) | (uint32_t(rr) << 16) | (uint32_t(rg) << 8) | uint32_t(rb);
    }
    void SetPixelRaw(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= 0 && x < w && y >= 0 && y < h) {
            pixels[y * w + x] = (0xFFu << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
    }
    static float SdRoundBox(float px, float py, float cx, float cy, float hw, float hh, float rad) {
        float dx = std::abs(px - cx) - (hw - rad);
        float dy = std::abs(py - cy) - (hh - rad);
        float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
        return std::hypot(ox, oy) + std::min(std::max(dx, dy), 0.0f) - rad;
    }
    static float SdSegment(float px, float py, float ax, float ay, float bx, float by) {
        float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
        float denom = bax * bax + bay * bay;
        float h = denom > 0.0f ? std::clamp((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
        return std::hypot(pax - bax * h, pay - bay * h);
    }
};


inline void Box(HDC dc,RECT r,COLORREF fill,COLORREF border,int diameter=8){
    AaCanvas canvas(dc, r, true);
    if (!canvas.Valid()) {
        HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,border);auto ob=SelectObject(dc,b);auto op=SelectObject(dc,p);
        RoundRect(dc,r.left,r.top,r.right,r.bottom,diameter,diameter);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);DeleteObject(p);
        return;
    }
    const float w = float(canvas.Width()), h = float(canvas.Height());
    const float hw = w * 0.5f - 0.5f, hh = h * 0.5f - 0.5f;
    const float rad = std::clamp(diameter * 0.5f, 0.0f, std::min(hw, hh));
    const float cx = w * 0.5f, cy = h * 0.5f;
    for (int y = 0; y < canvas.Height(); ++y) {
        for (int x = 0; x < canvas.Width(); ++x) {
            float d = AaCanvas::SdRoundBox(x + 0.5f, y + 0.5f, cx, cy, hw, hh, rad);
            float outerCov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (outerCov <= 0.0f) continue;
            if (fill == border) {
                canvas.BlendPixel(x, y, fill, outerCov);
            } else {
                float innerCov = std::clamp(0.5f - (d + 1.0f), 0.0f, 1.0f);
                canvas.BlendPixel(x, y, border, outerCov);
                if (innerCov > 0.0f) canvas.BlendPixel(x, y, fill, innerCov);
            }
        }
    }
}

inline void Circle(HDC dc, int centerX, int centerY, int radius, COLORREF fill, COLORREF border, int borderWidth = 2) {
    RECT r{ centerX - radius - 2, centerY - radius - 2, centerX + radius + 3, centerY + radius + 3 };
    AaCanvas canvas(dc, r, true);
    if (!canvas.Valid()) {
        HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,borderWidth,border);auto ob=SelectObject(dc,b);auto op=SelectObject(dc,p);
        Ellipse(dc,centerX-radius,centerY-radius,centerX+radius,centerY+radius);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);DeleteObject(p);
        return;
    }
    const float cx = float(centerX - r.left) + 0.5f, cy = float(centerY - r.top) + 0.5f;
    const float rad = float(radius);
    const float bw = float(borderWidth);
    for (int y = 0; y < canvas.Height(); ++y) {
        for (int x = 0; x < canvas.Width(); ++x) {
            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - rad;
            float outerCov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (outerCov <= 0.0f) continue;
            float innerCov = std::clamp(0.5f - (d + bw), 0.0f, 1.0f);
            canvas.BlendPixel(x, y, border, outerCov);
            if (innerCov > 0.0f) canvas.BlendPixel(x, y, fill, innerCov);
        }
    }
}

inline void Ring(HDC dc, int centerX, int centerY, int radius, int thickness, bool valid, int percent, COLORREF trackColor, COLORREF arcColor) {
    const int pad = thickness + 3;
    RECT r{ centerX - radius - pad, centerY - radius - pad, centerX + radius + pad + 1, centerY + radius + pad + 1 };
    AaCanvas canvas(dc, r, true);
    if (!canvas.Valid()) {
        HPEN p=CreatePen(PS_SOLID,thickness,trackColor);auto op=SelectObject(dc,p);auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Ellipse(dc,centerX-radius,centerY-radius,centerX+radius,centerY+radius);SelectObject(dc,op);DeleteObject(p);
        if(valid && percent>0) {
            p=CreatePen(PS_SOLID,thickness,arcColor);SelectObject(dc,p);POINT points[101];int count=std::clamp(percent,1,100);
            for(int n=0;n<=count;++n){double angle=-1.5707963267948966+6.283185307179586*n/100;points[n]={centerX+LONG(radius*std::cos(angle)),centerY+LONG(radius*std::sin(angle))};}
            Polyline(dc,points,count+1);SelectObject(dc,op);DeleteObject(p);
        }
        SelectObject(dc,ob);
        return;
    }
    const float cx = float(centerX - r.left), cy = float(centerY - r.top);
    const float rad = float(radius);
    const float halfT = std::max(1.0f, thickness * 0.5f);
    const int clampedPct = std::clamp(percent, 0, 100);
    const float sweep = clampedPct * 0.06283185307f; // 2*pi * pct / 100
    const float cap0x = cx, cap0y = cy - rad;
    const float cap1x = cx + rad * std::sin(sweep), cap1y = cy - rad * std::cos(sweep);

    for (int y = 0; y < canvas.Height(); ++y) {
        for (int x = 0; x < canvas.Width(); ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float distCenter = std::hypot(px - cx, py - cy);
            float ringDist = std::abs(distCenter - rad) - halfT;
            float trackCov = std::clamp(0.5f - ringDist, 0.0f, 1.0f);
            if (trackCov <= 0.0f) continue;
            canvas.BlendPixel(x, y, trackColor, trackCov);

            if (valid && clampedPct > 0) {
                if (clampedPct >= 100) {
                    canvas.BlendPixel(x, y, arcColor, trackCov);
                } else {
                    // Angle clockwise from 12 o'clock (0..2*pi)
                    float angle = std::atan2(px - cx, -(py - cy));
                    if (angle < 0.0f) angle += 6.283185307f;
                    float arcDist;
                    if (angle <= sweep) {
                        arcDist = ringDist;
                    } else {
                        float d0 = std::hypot(px - cap0x, py - cap0y) - halfT;
                        float d1 = std::hypot(px - cap1x, py - cap1y) - halfT;
                        arcDist = std::min(d0, d1);
                    }
                    float arcCov = std::clamp(0.5f - arcDist, 0.0f, 1.0f);
                    if (arcCov > 0.0f) canvas.BlendPixel(x, y, arcColor, arcCov);
                }
            }
        }
    }
}

inline void Surface(HDC dc,RECT r,int dpi){
    AaCanvas canvas(dc, r, false);
    if (!canvas.Valid()) {
        const LONG totalH=std::max(1L,r.bottom-r.top);
        for(int t=0;t<20;++t){
            LONG y0=r.top+(t*totalH+19)/20;
            LONG y1=t==19?r.bottom:(r.top+((t+1)*totalH+19)/20);
            if(y0>=y1)continue;
            HBRUSH b=CreateSolidBrush(RGB(36+t/5,50-t/2,71-t/3));
            RECT band{r.left,y0,r.right,y1};FillRect(dc,&band,b);DeleteObject(b);
        }
        HPEN p=CreatePen(PS_SOLID,1,Border);auto op=SelectObject(dc,p);auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));RoundRect(dc,r.left,r.top,r.right,r.bottom,Scale(22,dpi),Scale(22,dpi));SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(p);
        return;
    }
    const int w = canvas.Width(), h = canvas.Height();
    const int denom = std::max(1, h);
    for (int y = 0; y < h; ++y) {
        int t = (y * 20) / denom;
        uint8_t cr = uint8_t(36 + t / 5);
        uint8_t cg = uint8_t(50 - t / 2);
        uint8_t cb = uint8_t(71 - t / 3);
        for (int x = 0; x < w; ++x) {
            canvas.SetPixelRaw(x, y, cr, cg, cb);
        }
    }
    const float hw = w * 0.5f - 0.5f, hh = h * 0.5f - 0.5f;
    const float rad = std::clamp(Scale(22, dpi) * 0.5f, 0.0f, std::min(hw, hh));
    const float cx = w * 0.5f, cy = h * 0.5f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Only evaluate SDF near the perimeter or corners for fast rendering
            if (x > int(rad + 2) && x < w - int(rad + 3) && y > 2 && y < h - 3) continue;
            float d = AaCanvas::SdRoundBox(x + 0.5f, y + 0.5f, cx, cy, hw, hh, rad);
            float borderBand = 1.0f - std::abs(d + 0.5f);
            if (borderBand > 0.0f) {
                canvas.BlendPixel(x, y, Border, std::clamp(borderBand, 0.0f, 1.0f));
            }
        }
    }
}

inline void Label(HDC dc,const wchar_t* text,RECT r,int size,int dpi,COLORREF color=Text,UINT align=DT_LEFT,bool bold=false){
    HFONT f=Font(size,dpi,bold);auto old=SelectObject(dc,f);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,text,-1,&r,align|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);SelectObject(dc,old);DeleteObject(f);
}
inline void Connection(HDC dc,RECT r,int dpi,bool connected,bool wired){
    Box(dc,r,Control,Border,Scale(8,dpi));Label(dc,!connected?L"未连接":wired?L"USB":L"2.4G",r,12,dpi,Muted,DT_CENTER);
}
inline void DeviceIcon(HDC dc,int x,int y,int dpi,bool keyboard){
    auto S=[dpi](int n){return Scale(n,dpi);};
    COLORREF col = keyboard ? Keyboard : Accent;
    RECT bounds{ x - 2, y - 2, x + S(27) + 2, y + S(28) + 2 };
    AaCanvas canvas(dc, bounds, true);
    if (!canvas.Valid()) {
        HPEN p=CreatePen(PS_SOLID,std::max(1,S(1)),col);auto op=SelectObject(dc,p);auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        if(keyboard){RoundRect(dc,x,y+S(5),x+S(25),y+S(22),S(4),S(4));for(int row=0;row<2;++row)for(int colIdx=0;colIdx<4;++colIdx){MoveToEx(dc,x+S(4+colIdx*5),y+S(10+row*5),nullptr);LineTo(dc,x+S(6+colIdx*5),y+S(10+row*5));}}
        else{RoundRect(dc,x+S(5),y,x+S(20),y+S(26),S(12),S(12));MoveToEx(dc,x+S(12),y+S(3),nullptr);LineTo(dc,x+S(12),y+S(10));}
        SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(p);
        return;
    }
    const float ox = float(x - bounds.left), oy = float(y - bounds.top);
    const float halfStroke = std::max(0.65f, S(1) * 0.55f);
    for (int py = 0; py < canvas.Height(); ++py) {
        for (int px = 0; px < canvas.Width(); ++px) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float minDist = 1000.0f;
            if (keyboard) {
                float boxD = std::abs(AaCanvas::SdRoundBox(fx, fy, ox + S(25) * 0.5f, oy + S(5) + S(17) * 0.5f, S(25) * 0.5f, S(17) * 0.5f, S(2)));
                minDist = std::min(minDist, boxD);
                for (int row = 0; row < 2; ++row) {
                    for (int c = 0; c < 4; ++c) {
                        float segD = AaCanvas::SdSegment(fx, fy, ox + S(4 + c * 5), oy + S(10 + row * 5), ox + S(6 + c * 5), oy + S(10 + row * 5));
                        minDist = std::min(minDist, segD);
                    }
                }
            } else {
                float bodyD = std::abs(AaCanvas::SdRoundBox(fx, fy, ox + S(5) + S(15) * 0.5f, oy + S(26) * 0.5f, S(15) * 0.5f, S(26) * 0.5f, S(6)));
                float wheelD = AaCanvas::SdSegment(fx, fy, ox + S(12.5f), oy + S(4), ox + S(12.5f), oy + S(10));
                minDist = std::min(bodyD, wheelD);
            }
            float cov = std::clamp(0.5f - (minDist - halfStroke), 0.0f, 1.0f);
            if (cov > 0.0f) canvas.BlendPixel(px, py, col, cov);
        }
    }
}
inline void ChargeIcon(HDC dc,int x,int y,int dpi){
    auto S=[dpi](int n){return Scale(n,dpi);};
    RECT bounds{ x - 2, y - 2, x + S(14) + 2, y + S(20) + 2 };
    AaCanvas canvas(dc, bounds, true);
    if (!canvas.Valid()) {
        POINT points[]={{x+S(8),y},{x,y+S(10)},{x+S(5),y+S(10)},{x+S(2),y+S(18)},{x+S(12),y+S(7)},{x+S(7),y+S(7)}};
        HBRUSH b=CreateSolidBrush(Accent);auto ob=SelectObject(dc,b);auto op=SelectObject(dc,GetStockObject(NULL_PEN));Polygon(dc,points,6);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);
        return;
    }
    const float ox = float(x - bounds.left), oy = float(y - bounds.top);
    const float vx[6] = { ox + S(8), ox, ox + S(5), ox + S(2), ox + S(12), ox + S(7) };
    const float vy[6] = { oy, oy + S(10), oy + S(10), oy + S(18), oy + S(7), oy + S(7) };
    // 4x4 supersampled polygon coverage for razor-sharp lightning bolt
    for (int py = 0; py < canvas.Height(); ++py) {
        for (int px = 0; px < canvas.Width(); ++px) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                float fy = py + (sy + 0.5f) * 0.25f;
                for (int sx = 0; sx < 4; ++sx) {
                    float fx = px + (sx + 0.5f) * 0.25f;
                    bool inside = false;
                    for (int i = 0, j = 5; i < 6; j = i++) {
                        if (((vy[i] > fy) != (vy[j] > fy)) &&
                            (fx < (vx[j] - vx[i]) * (fy - vy[i]) / (vy[j] - vy[i]) + vx[i])) {
                            inside = !inside;
                        }
                    }
                    if (inside) ++hits;
                }
            }
            if (hits > 0) canvas.BlendPixel(px, py, Accent, hits / 16.0f);
        }
    }
}
}
