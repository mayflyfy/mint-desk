#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace desk {

// One per-user Explorer desktop verb. No extension DLL or elevation.
bool SetDesktopMenu(bool enabled);
bool IsDesktopMenuRegistered();
// Updates the existing verb title. Never enables an unregistered menu.
bool RefreshDesktopMenu(bool running);

namespace testing {
// shellParent must be an isolated test key, never the real Explorer Shell key.
// Only the fixed child key "MintDesk" is changed; sibling keys are untouched.
bool SetDesktopMenuAtKey(HKEY shellParent, const std::wstring& executable, bool enabled,
    bool running = false);
bool IsDesktopMenuRegisteredAtKey(HKEY shellParent, const std::wstring& executable);
bool RefreshDesktopMenuAtKey(HKEY shellParent, const std::wstring& executable, bool running);
}

} // namespace desk
