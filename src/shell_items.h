#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace desk {

struct ShellVisual {
    // The caller owns this icon and must release it with DestroyIcon.
    HICON icon = nullptr;
    std::wstring label;
};

// Shell icon extraction may load shell extensions or access a remote path.
// Call this on a worker thread, rather than from a window paint handler.
ShellVisual LoadShellVisual(const std::wstring& path, int pixelSize);

bool OpenItem(HWND owner, const std::wstring& path);
bool RevealItem(HWND owner, const std::wstring& path);
bool ShowItemProperties(HWND owner, const std::wstring& path);

// Opens a deliberate native picker. folders=false accepts files of any type.
std::vector<std::wstring> PickItems(HWND owner, bool folders);
std::vector<std::wstring> PathsFromDrop(HDROP drop);

// Pure lexical validation: never checks existence or probes a network share.
bool IsSupportedPath(const std::wstring& path);

} // namespace desk
