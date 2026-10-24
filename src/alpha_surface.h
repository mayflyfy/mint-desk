#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

namespace desk {

// Draw the same foreground onto both matte DCs after Create(). Compose replaces
// the black matte with a premultiplied BGRA image; Create clears both for reuse.
// GDI text must use grayscale antialiasing rather than ClearType.
class AlphaSurface {
public:
    AlphaSurface();
    ~AlphaSurface();
    AlphaSurface(const AlphaSurface&) = delete;
    AlphaSurface& operator=(const AlphaSurface&) = delete;

    bool Create(int width, int height);
    HDC BlackDC() const { return blackDc_; }
    HDC WhiteDC() const { return whiteDc_; }
    bool Compose(COLORREF background, BYTE backgroundAlpha);
    bool Present(HWND window, POINT screenPosition);
    bool BlitOver(HDC target, int x = 0, int y = 0);
    const uint32_t* Pixels() const { return blackPixels_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    void Reset();
    HDC blackDc_ = nullptr, whiteDc_ = nullptr;
    HBITMAP blackBitmap_ = nullptr, whiteBitmap_ = nullptr;
    HGDIOBJ oldBlack_ = nullptr, oldWhite_ = nullptr;
    uint32_t* blackPixels_ = nullptr;
    uint32_t* whitePixels_ = nullptr;
    int width_ = 0, height_ = 0;
    bool composed_ = false;
};

} // namespace desk
