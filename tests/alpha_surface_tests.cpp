#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include "../src/alpha_surface.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "AlphaSurface test failed: %s (Win32 %lu)\n", message, GetLastError());
        std::exit(1);
    }
}
unsigned A(uint32_t p) { return p >> 24; }
unsigned R(uint32_t p) { return (p >> 16) & 255u; }
unsigned G(uint32_t p) { return (p >> 8) & 255u; }
unsigned B(uint32_t p) { return p & 255u; }
void CheckPremultiplied(const desk::AlphaSurface& surface) {
    for (int i = 0; i < surface.Width() * surface.Height(); ++i) {
        const auto p = surface.Pixels()[i];
        Check(R(p) <= A(p) && G(p) <= A(p) && B(p) <= A(p), "premultiplied channels");
    }
}
void OpaqueForegroundAndEmptyBackground() {
    desk::AlphaSurface surface;
    Check(!surface.Create(0, 8) && !surface.Create(8, -1), "reject invalid dimensions");
    Check(!surface.Create(2000000000, 2000000000), "reject overflowing dimensions");
    Check(!surface.Compose(RGB(20, 40, 60), 179), "reject uninitialized compose");
    for (const BYTE opacity : {BYTE(0), BYTE(179), BYTE(255)}) {
        Check(surface.Create(12, 9), "create/reuse");
        HBRUSH brush = CreateSolidBrush(RGB(213, 88, 37));
        RECT box{2, 2, 6, 6};
        FillRect(surface.BlackDC(), &box, brush);
        FillRect(surface.WhiteDC(), &box, brush);
        DeleteObject(brush);
        Check(surface.Compose(RGB(20, 40, 60), opacity), "compose rectangle");
        Check(surface.Pixels()[3 * 12 + 3] == 0xffd55825u,
            "opaque foreground remains exact at every background opacity");
        const auto empty = surface.Pixels()[0];
        Check(A(empty) == opacity, "background alpha");
        Check(R(empty) == (20u * opacity + 127u) / 255u &&
            G(empty) == (40u * opacity + 127u) / 255u &&
            B(empty) == (60u * opacity + 127u) / 255u,
            "premultiplied background color");
        CheckPremultiplied(surface);
        Check(!surface.Compose(RGB(20, 40, 60), opacity), "prevent composing consumed matte");
    }
}
void PartialCoverage() {
    desk::AlphaSurface surface;
    Check(surface.Create(3, 3), "create antialias test");
    SetPixelV(surface.BlackDC(), 1, 1, RGB(50, 25, 10));
    SetPixelV(surface.WhiteDC(), 1, 1, RGB(177, 152, 137));
    Check(surface.Compose(RGB(20, 40, 60), 179), "compose partial coverage");
    const uint32_t expected = (217u << 24) | (57u << 16) | (39u << 8) | 31u;
    Check(surface.Pixels()[4] == expected, "partial foreground over translucent background");
    CheckPremultiplied(surface);
}
void Text() {
    desk::AlphaSurface surface;
    Check(surface.Create(320, 48), "create text test");
    HFONT font = CreateFontW(-25, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    Check(font != nullptr, "create grayscale font");
    for (HDC dc : {surface.BlackDC(), surface.WhiteDC()}) {
        const auto old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 235, 241));
        RECT rect{4, 0, 316, 48};
        Check(DrawTextW(dc, L"MintDesk 常用", -1, &rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) != 0, "draw real text");
        SelectObject(dc, old);
    }
    DeleteObject(font);
    Check(surface.Compose(RGB(20, 24, 30), 0), "compose text");
    size_t solid = 0, antialiased = 0;
    for (int i = 0; i < surface.Width() * surface.Height(); ++i) {
        const auto pixel = surface.Pixels()[i];
        if (pixel == 0xffe6ebf1u) ++solid;
        if (A(pixel) > 0 && A(pixel) < 255) ++antialiased;
    }
    Check(solid > 5, "text retains original solid color");
    Check(antialiased > 5, "real grayscale-antialiased edges reconstructed");
    CheckPremultiplied(surface);
}
void Icons() {
    desk::AlphaSurface iconSource;
    Check(iconSource.Create(8, 8), "create icon source");
    auto pixels = const_cast<uint32_t*>(iconSource.Pixels());
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        pixels[y * 8 + x] = x >= 2 && x <= 5 && y >= 2 && y <= 5
            ? 0xff32c864u : 0u;
    }
    HBITMAP color = static_cast<HBITMAP>(GetCurrentObject(iconSource.BlackDC(), OBJ_BITMAP));
    HBITMAP mask = CreateBitmap(8, 8, 1, 1, nullptr);
    Check(mask != nullptr, "create icon mask");
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    Check(icon != nullptr, "create alpha icon");
    for (const BYTE opacity : {BYTE(0), BYTE(179), BYTE(255)}) {
        desk::AlphaSurface surface;
        Check(surface.Create(12, 12), "create icon target");
        Check(DrawIconEx(surface.BlackDC(), 2, 2, icon, 8, 8, 0, nullptr, DI_NORMAL) != FALSE,
            "draw real icon black matte");
        Check(DrawIconEx(surface.WhiteDC(), 2, 2, icon, 8, 8, 0, nullptr, DI_NORMAL) != FALSE,
            "draw real icon white matte");
        Check(surface.Compose(RGB(20, 24, 30), opacity), "compose real icon");
        Check(surface.Pixels()[5 * 12 + 5] == 0xff32c864u, "icon retains original color");
        Check(A(surface.Pixels()[0]) == opacity, "icon background opacity independent");
        CheckPremultiplied(surface);
    }
    DestroyIcon(icon);
}
void PresentationAndPreview() {
    desk::AlphaSurface surface, target;
    Check(surface.Create(16, 16) && target.Create(24, 24), "create presentation surfaces");
    Check(surface.Compose(RGB(100, 50, 20), 128), "compose presentation");
    RECT rect{0, 0, 24, 24};
    HBRUSH brush = CreateSolidBrush(RGB(10, 20, 30));
    FillRect(target.BlackDC(), &rect, brush);
    DeleteObject(brush);
    Check(surface.BlitOver(target.BlackDC(), 2, 2), "AlphaBlend preview over existing canvas");
    GdiFlush();
    const COLORREF preview = GetPixel(target.BlackDC(), 3, 3);
    Check(GetRValue(preview) == 55 && GetGValue(preview) == 35 && GetBValue(preview) == 25,
        "preview compositing matches source alpha");
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"MintDesk alpha test", WS_POPUP, 0, 0, 16, 16,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden layered window");
    Check(surface.Present(window, POINT{0, 0}), "UpdateLayeredWindow presentation");
    DestroyWindow(window);
}
} // namespace

int main() {
    OpaqueForegroundAndEmptyBackground();
    PartialCoverage();
    Text();
    Icons();
    PresentationAndPreview();
    std::puts("alpha surface tests passed");
    return 0;
}
