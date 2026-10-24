#include "../src/shell_items.h"

#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
    }
}

void CheckIcon(desk::ShellVisual visual, int expectedPixels, const char* label) {
    Check(visual.icon != nullptr, label);
    if (!visual.icon) return;
    ICONINFO info{};
    Check(GetIconInfo(visual.icon, &info) != FALSE, "icon can be inspected");
    if (info.hbmColor) {
        BITMAP bitmap{};
        Check(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0 &&
            bitmap.bmWidth == expectedPixels && bitmap.bmHeight == expectedPixels,
            "icon uses requested physical size");
        DeleteObject(info.hbmColor);
    }
    if (info.hbmMask) DeleteObject(info.hbmMask);
    Check(DestroyIcon(visual.icon) != FALSE, "caller owns the returned icon");
}

void CheckDrop() {
    const std::vector<std::wstring> expected = {
        L"D:\\\u9879\u76ee\\\u5de5\u5177 \U0001F4BB.lnk",
        L"\\\\mintdesk-never-connect.invalid\\share\\\u6587\u4ef6.txt"
    };
    std::vector<wchar_t> names;
    for (const auto& path : expected) {
        names.insert(names.end(), path.begin(), path.end());
        names.push_back(L'\0');
    }
    const wchar_t rejected[] = L"relative.txt\0https://example.invalid\0";
    names.insert(names.end(), rejected, rejected + sizeof(rejected) / sizeof(wchar_t));
    // The final string literal terminator supplies the second terminating NUL.
    const SIZE_T bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GHND, bytes);
    Check(memory != nullptr, "allocate synthetic Unicode file drop");
    if (!memory) return;
    char* raw = static_cast<char*>(GlobalLock(memory));
    Check(raw != nullptr, "lock synthetic Unicode file drop");
    if (!raw) { GlobalFree(memory); return; }
    auto* header = reinterpret_cast<DROPFILES*>(raw);
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    std::memcpy(raw + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    HDROP drop = reinterpret_cast<HDROP>(memory);
    Check(desk::PathsFromDrop(drop) == expected,
        "Unicode file drop preserves order and filters unsupported references");
    DragFinish(drop);
    Check(desk::PathsFromDrop(nullptr).empty(), "empty file drop");
}
} // namespace

int main() {
    using desk::IsSupportedPath;
    Check(IsSupportedPath(L"C:\\Windows\\explorer.exe"), "absolute drive path");
    Check(IsSupportedPath(L"D:/folder/file.lnk"), "forward slash drive path");
    Check(IsSupportedPath(L"D:\\\u6587\u4ef6\\\u5de5\u5177 \U0001F4BB.lnk"), "Unicode path");
    Check(IsSupportedPath(L"\\\\mintdesk-never-connect.invalid\\share\\file.txt"), "UNC path is lexical");
    Check(IsSupportedPath(L"//mintdesk-never-connect.invalid/share/file.txt"), "forward slash UNC path");
    Check(!IsSupportedPath(L"relative.txt"), "reject relative path");
    Check(!IsSupportedPath(L"C:relative.txt"), "reject drive-relative path");
    Check(!IsSupportedPath(L"https://example.invalid"), "reject URL command");
    Check(!IsSupportedPath(L"C:\\file.txt:stream"), "reject alternate data stream");
    Check(!IsSupportedPath(L"\\\\.\\C:"), "reject device namespace");
    Check(!IsSupportedPath(std::wstring(L"C:\\bad\0hidden", 13)), "reject embedded NUL");
    Check(!IsSupportedPath(L"C:\\bad\nname"), "reject control character");
    Check(!IsSupportedPath(L"C:\\bad*name"), "reject wildcard");
    Check(!IsSupportedPath(L"\\\\server"), "reject missing UNC share");
    Check(!IsSupportedPath(L"\\\\server\\"), "reject empty UNC share");
    CheckDrop();

    // None of these checks launches an item, opens UI, creates a file, or asks
    // the network server to resolve a file. UNC visuals only use generic icons.
    const auto remote = desk::LoadShellVisual(
        L"\\\\mintdesk-never-connect.invalid\\share\\\u5de5\u5177.LNK", 48);
    Check(remote.label == L"\u5de5\u5177", "UNC shortcut keeps lexical display label");
    CheckIcon(remote, 48, "generic UNC icon");
    const auto forwardRemote = desk::LoadShellVisual(
        L"//mintdesk-never-connect.invalid/share/Example.URL", 20);
    Check(forwardRemote.label == L"Example", "forward slash UNC label");
    CheckIcon(forwardRemote, 20, "generic forward slash UNC icon");
    const auto remoteFolder = desk::LoadShellVisual(
        L"\\\\mintdesk-never-connect.invalid\\share\\folder\\", 16);
    Check(remoteFolder.label == L"folder", "trailing slash UNC folder label");
    CheckIcon(remoteFolder, 16, "generic UNC folder icon");

    wchar_t windows[MAX_PATH]{};
    Check(GetWindowsDirectoryW(windows, MAX_PATH) != 0, "locate Windows directory");
    const auto local = desk::LoadShellVisual(std::wstring(windows) + L"\\explorer.exe", 48);
    Check(!local.label.empty(), "real local item gets display label");
    CheckIcon(local, 48, "real local item icon");
    const auto missing = desk::LoadShellVisual(std::wstring(windows) +
        L"\\MintDesk-nonexistent-shell-test-" + std::to_wstring(GetCurrentProcessId()) + L".txt", 20);
    Check(!missing.label.empty(), "missing local item keeps display label");
    CheckIcon(missing, 20, "missing local item gets generic icon");
    const auto invalid = desk::LoadShellVisual(L"https://example.invalid", 48);
    Check(!invalid.icon, "invalid reference never invokes shell icon lookup");

    if (!failures) std::puts("Shell item tests passed.");
    return failures ? 1 : 0;
}
