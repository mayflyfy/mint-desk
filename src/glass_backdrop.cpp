#include "glass_backdrop.h"

#include <dwmapi.h>
#include <cstring>

namespace desk {
namespace {
constexpr wchar_t kBackdropClass[] = L"MintDesk.Backdrop.1";
constexpr int kAccentPolicyAttribute = 19;
constexpr int kAccentDisabled = 0;
constexpr int kAccentBlurBehind = 3;

struct AccentPolicy {
    int state;
    DWORD flags;
    DWORD gradientColor;
    DWORD animationId;
};
struct CompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T size;
};
using SetCompositionAttribute = BOOL(WINAPI*)(HWND, CompositionAttributeData*);

SetCompositionAttribute CompositionFunction() {
    // The accent policy is not a supported SDK contract. Resolve it at runtime
    // so unavailable implementations simply use the existing translucent tint.
    static const auto function = [] {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        FARPROC address = user ? GetProcAddress(user, "SetWindowCompositionAttribute") : nullptr;
        SetCompositionAttribute result = nullptr;
        static_assert(sizeof(result) == sizeof(address), "Windows procedure pointer size");
        // Avoid incompatible-function-pointer warnings in older MinGW headers.
        memcpy(&result, &address, sizeof(result));
        return result;
    }();
    return function;
}

bool SystemAllowsBlur() {
    BOOL composed = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&composed)) || !composed || GetSystemMetrics(SM_REMOTESESSION)) return false;
    HIGHCONTRASTW contrast{}; contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return false;
    DWORD transparency = 1, size = sizeof(transparency);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size) == ERROR_SUCCESS && !transparency) return false;
    SYSTEM_POWER_STATUS power{};
    // Older MinGW SDKs call the fourth byte Reserved1; Win10 defines it as
    // SystemStatusFlag (battery-saver status) without changing the ABI.
    if (GetSystemPowerStatus(&power) && reinterpret_cast<const BYTE*>(&power)[3] == 1) return false;
    return true;
}

} // namespace

GlassBackdrop::~GlassBackdrop() { Destroy(); }

bool GlassBackdrop::Create(HWND card) {
    Destroy();
    if (!IsWindow(card)) return false;
    card_ = card;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW info{}; info.cbSize = sizeof(info);
    if (!GetClassInfoExW(instance, kBackdropClass, &info)) {
        info = {}; info.cbSize = sizeof(info); info.lpfnWndProc = WindowProc;
        info.hInstance = instance; info.lpszClassName = kBackdropClass;
        // Black GDI pixels have zero alpha in the DWM-extended client surface.
        info.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassExW(&info)) { card_ = nullptr; return false; }
    }
    RECT rect{};
    if (!GetWindowRect(card, &rect)) { card_ = nullptr; return false; }
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kBackdropClass, L"", WS_POPUP | WS_DISABLED,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, this);
    if (!window_) { card_ = nullptr; return false; }
    // Extend the compositor surface throughout the empty helper window. This
    // does not itself request blur on Win10; the accent policy below does.
    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(window_, &margins);
    return true;
}

bool GlassBackdrop::Apply(bool enabled) {
    requested_ = enabled;
    accepted_ = false;
    if (!IsWindow(window_)) return false;
    const auto function = CompositionFunction();
    if (function) {
        bool allowed = enabled && SystemAllowsBlur();
        if (allowed) {
            // Frame extension must be renewed after compositor transitions.
            // If it cannot supply a transparent surface, hide the helper rather
            // than leaving an opaque black rectangle behind the foreground.
            const MARGINS margins{-1, -1, -1, -1};
            allowed = SUCCEEDED(DwmExtendFrameIntoClientArea(window_, &margins));
        }
        AccentPolicy policy{allowed ? kAccentBlurBehind : kAccentDisabled, 0, 0, 0};
        CompositionAttributeData data{kAccentPolicyAttribute, &policy, sizeof(policy)};
        accepted_ = function(window_, &data) != FALSE && allowed;
    }
    RECT rect{};
    if (IsWindow(card_) && GetWindowRect(card_, &rect)) Sync(rect, visible_);
    else ShowWindow(window_, SW_HIDE);
    return accepted_;
}

void GlassBackdrop::Sync(const RECT& rect, bool visible) {
    visible_ = visible;
    if (!IsWindow(window_) || !IsWindow(card_)) return;
    const bool show = visible && requested_ && accepted_ && IsWindowVisible(card_) && !IsIconic(card_);
    if (!show) { ShowWindow(window_, SW_HIDE); return; }
    RECT current{};
    const bool topmost=(GetWindowLongPtrW(window_,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
    if(!topmost&&IsWindowVisible(window_)&&GetWindow(card_,GW_HWNDNEXT)==window_&&
        GetWindowRect(window_,&current)&&EqualRect(&current,&rect))return;
    syncing_=true;
    if(topmost)SetWindowPos(window_,HWND_BOTTOM,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOSENDCHANGING);
    SetWindowPos(window_, card_, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    syncing_=false;
}

void GlassBackdrop::Destroy() {
    HWND window = window_;
    window_ = nullptr;
    card_ = nullptr;
    requested_ = accepted_ = visible_ = false;
    syncing_=false;
    if (IsWindow(window)) DestroyWindow(window);
}

LRESULT CALLBACK GlassBackdrop::WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<GlassBackdrop*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<GlassBackdrop*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (message) {
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        if (dc) FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(window, &paint); return 0;
    }
    case WM_WINDOWPOSCHANGING:
        if (self && IsWindow(self->card_)) {
            auto* position = reinterpret_cast<WINDOWPOS*>(lp);
            if (!(position->flags & SWP_NOZORDER)) position->hwndInsertAfter = self->card_;
            RECT rect{};
            if (GetWindowRect(self->card_, &rect)) {
                if (!(position->flags & SWP_NOMOVE)) { position->x = rect.left; position->y = rect.top; }
                if (!(position->flags & SWP_NOSIZE)) { position->cx = rect.right - rect.left; position->cy = rect.bottom - rect.top; }
            }
            position->flags |= SWP_NOACTIVATE;
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if(self&&!self->syncing_&&IsWindowVisible(window)&&IsWindow(self->card_)&&
            ((GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)||
                GetWindow(self->card_,GW_HWNDNEXT)!=window||!IsWindowVisible(self->card_)))
            PostMessageW(self->card_,kBackdropOrderChanged,0,0);
        break;
    case WM_DWMCOMPOSITIONCHANGED:
    case WM_SETTINGCHANGE:
    case WM_POWERBROADCAST:
        if (self && self->window_ == window) self->Apply(self->requested_);
        return message == WM_POWERBROADCAST ? TRUE : 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (self && self->window_ == window) self->window_ = nullptr;
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

} // namespace desk
