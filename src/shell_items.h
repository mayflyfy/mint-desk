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

struct ShellItemStatus {
    bool missing = false;
    std::vector<std::wstring> watchDirectories;
};

// Worker-thread inspection. Only definite absence on a fixed local drive is
// considered missing; unavailable, remote and removable references are retained.
// Shortcuts are read without resolving, repairing or launching their targets.
ShellItemStatus InspectShellItem(const std::wstring& path);

class ShellChangeWatch {
public:
    ShellChangeWatch() = default;
    ShellChangeWatch(const ShellChangeWatch&) = delete;
    ShellChangeWatch& operator=(const ShellChangeWatch&) = delete;
    ~ShellChangeWatch();
    void Update(HWND window, UINT message, std::vector<std::wstring> directories);
    void Clear();
private:
    struct Registration { std::wstring directory; ULONG id; };
    std::vector<Registration> registrations_;
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
