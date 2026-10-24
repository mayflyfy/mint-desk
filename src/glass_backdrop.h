#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace desk {

// A native, ownerless blur-only window, immediately behind the card's existing
// per-pixel-alpha foreground. No timer, capture, shell hook, or renderer thread.
class GlassBackdrop {
public:
    GlassBackdrop() = default;
    ~GlassBackdrop();
    GlassBackdrop(const GlassBackdrop&) = delete;
    GlassBackdrop& operator=(const GlassBackdrop&) = delete;

    bool Create(HWND card);
    // True means Windows accepted the blur request. Windows may still alter
    // its visual result according to compositor policy and the OS version.
    bool Apply(bool enabled);
    void Sync(const RECT& cardRect, bool visible);
    void Destroy();
    HWND Window() const { return window_; }
    bool BlurAccepted() const { return accepted_; }

private:
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    HWND window_ = nullptr;
    HWND card_ = nullptr;
    bool requested_ = false;
    bool accepted_ = false;
    bool visible_ = false;
};

} // namespace desk
