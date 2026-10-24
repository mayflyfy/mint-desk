#include "alpha_surface.h"
#include <algorithm>
#include <cstddef>

namespace desk {
namespace {
constexpr int kMaxDimension = 16384;
constexpr size_t kMaxPixels = 64u * 1024u * 1024u;

unsigned Channel(uint32_t pixel, unsigned shift) {
    return (pixel >> shift) & 255u;
}

// The black matte already contains the premultiplied foreground contribution.
unsigned CompositeChannel(unsigned foreground, unsigned background,
    unsigned backgroundAlpha, unsigned uncovered, unsigned resultAlpha) {
    const unsigned contribution =
        (background * backgroundAlpha * uncovered + 32512u) / 65025u;
    return std::min(resultAlpha, foreground + contribution);
}
} // namespace

AlphaSurface::AlphaSurface() = default;
AlphaSurface::~AlphaSurface() { Reset(); }

void AlphaSurface::Reset() {
    if (blackDc_ && oldBlack_) SelectObject(blackDc_, oldBlack_);
    if (whiteDc_ && oldWhite_) SelectObject(whiteDc_, oldWhite_);
    if (blackBitmap_) DeleteObject(blackBitmap_);
    if (whiteBitmap_) DeleteObject(whiteBitmap_);
    if (blackDc_) DeleteDC(blackDc_);
    if (whiteDc_) DeleteDC(whiteDc_);
    blackDc_ = whiteDc_ = nullptr;
    blackBitmap_ = whiteBitmap_ = nullptr;
    oldBlack_ = oldWhite_ = nullptr;
    blackPixels_ = whitePixels_ = nullptr;
    width_ = height_ = 0;
    composed_ = false;
}

bool AlphaSurface::Create(int width, int height) {
    if (width <= 0 || height <= 0 || width > kMaxDimension ||
        height > kMaxDimension ||
        static_cast<size_t>(width) * static_cast<size_t>(height) > kMaxPixels)
        return false;

    if (width != width_ || height != height_ || !blackPixels_ || !whitePixels_) {
        Reset();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        blackDc_ = CreateCompatibleDC(nullptr);
        whiteDc_ = CreateCompatibleDC(nullptr);
        if (!blackDc_ || !whiteDc_) { Reset(); return false; }
        void* blackBits = nullptr;
        void* whiteBits = nullptr;
        blackBitmap_ = CreateDIBSection(blackDc_, &info, DIB_RGB_COLORS,
            &blackBits, nullptr, 0);
        whiteBitmap_ = CreateDIBSection(whiteDc_, &info, DIB_RGB_COLORS,
            &whiteBits, nullptr, 0);
        if (!blackBitmap_ || !whiteBitmap_ || !blackBits || !whiteBits) {
            Reset(); return false;
        }
        oldBlack_ = SelectObject(blackDc_, blackBitmap_);
        oldWhite_ = SelectObject(whiteDc_, whiteBitmap_);
        if (!oldBlack_ || oldBlack_ == HGDI_ERROR ||
            !oldWhite_ || oldWhite_ == HGDI_ERROR) {
            if (oldBlack_ == HGDI_ERROR) oldBlack_ = nullptr;
            if (oldWhite_ == HGDI_ERROR) oldWhite_ = nullptr;
            Reset(); return false;
        }
        blackPixels_ = static_cast<uint32_t*>(blackBits);
        whitePixels_ = static_cast<uint32_t*>(whiteBits);
        width_ = width;
        height_ = height;
    }
    GdiFlush();
    const size_t count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    std::fill_n(blackPixels_, count, 0u);
    std::fill_n(whitePixels_, count, 0x00ffffffu);
    composed_ = false;
    return true;
}

bool AlphaSurface::Compose(COLORREF background, BYTE backgroundAlpha) {
    if (!blackPixels_ || !whitePixels_ || composed_) return false;
    GdiFlush();
    const unsigned bgR = GetRValue(background), bgG = GetGValue(background);
    const unsigned bgB = GetBValue(background);
    const size_t count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t black = blackPixels_[i], white = whitePixels_[i];
        const unsigned blackB = Channel(black, 0), blackG = Channel(black, 8);
        const unsigned blackR = Channel(black, 16);
        const unsigned diffB = Channel(white, 0) > blackB ? Channel(white, 0) - blackB : 0;
        const unsigned diffG = Channel(white, 8) > blackG ? Channel(white, 8) - blackG : 0;
        const unsigned diffR = Channel(white, 16) > blackR ? Channel(white, 16) - blackR : 0;
        // A single coverage value avoids tinting grayscale-antialiased text.
        const unsigned uncovered = std::min({diffB, diffG, diffR});
        const unsigned foregroundAlpha = 255u - uncovered;
        const unsigned resultAlpha = foregroundAlpha +
            (static_cast<unsigned>(backgroundAlpha) * uncovered + 127u) / 255u;
        const unsigned outB = CompositeChannel(blackB, bgB, backgroundAlpha, uncovered, resultAlpha);
        const unsigned outG = CompositeChannel(blackG, bgG, backgroundAlpha, uncovered, resultAlpha);
        const unsigned outR = CompositeChannel(blackR, bgR, backgroundAlpha, uncovered, resultAlpha);
        blackPixels_[i] = (resultAlpha << 24) | (outR << 16) | (outG << 8) | outB;
    }
    composed_ = true;
    return true;
}

bool AlphaSurface::Present(HWND window, POINT screenPosition) {
    if (!window || !blackDc_ || !composed_) return false;
    SIZE size{width_, height_};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return UpdateLayeredWindow(window, nullptr, &screenPosition, &size, blackDc_,
        &source, 0, &blend, ULW_ALPHA) != FALSE;
}

bool AlphaSurface::BlitOver(HDC target, int x, int y) {
    if (!target || !blackDc_ || !composed_) return false;
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return AlphaBlend(target, x, y, width_, height_, blackDc_, 0, 0,
        width_, height_, blend) != FALSE;
}

} // namespace desk
