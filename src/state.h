#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace desk {

enum class Layout : uint32_t { Grid = 0, List = 1 };

// All coordinates and sizes are DIPs, relative to the selected work area.
struct CardState {
    uint32_t id = 0;
    std::wstring title;
    std::wstring monitor;
    // Height always records the expanded size, including while the card is collapsed.
    int x = 0, y = 0, width = 340, height = 420;
    Layout layout = Layout::Grid;
    bool locked = false;
    bool collapsed = false;
    std::vector<std::wstring> paths;
};

struct AppState {
    uint32_t nextId = 1;
    // Zero selects the default tinted glass; other values set background alpha.
    // Icons and text remain fully opaque.
    BYTE opacity = 0;
    std::vector<CardState> cards;
};

struct MonitorInfo {
    std::wstring device;
    RECT work{};
    UINT dpi = 96;
};

std::vector<MonitorInfo> EnumerateMonitors();
RECT ResolveCardRect(const CardState&, const std::vector<MonitorInfo>&);
void CaptureCardRect(CardState&, HWND);
std::wstring DataDirectory();
bool LoadState(AppState&);
bool SaveState(const AppState&);
bool SetAutostart(bool);
bool IsAutostartEnabled();
std::wstring ExecutablePath();

// Explicit directory injection is limited to callers running isolated tests.
// Production never reads a directory override from the environment or command line.
namespace testing {
std::wstring DataDirectoryUnder(const std::wstring& local);
bool LoadStateFromDirectory(const std::wstring& directory, AppState&);
bool SaveStateToDirectory(const std::wstring& directory, const AppState&);
}

} // namespace desk
