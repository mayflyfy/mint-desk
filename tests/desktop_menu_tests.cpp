#include "../src/desktop_menu.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
std::wstring Read(HKEY root, const wchar_t* path, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    LONG result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    std::wstring value;
    if (result == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
        value.resize(bytes / sizeof(wchar_t));
        result = RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value[0]), &bytes);
        if (result == ERROR_SUCCESS && value.back() == L'\0') value.pop_back();
        else value.clear();
    }
    RegCloseKey(key);
    return value;
}
bool Set(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr,
        &key, nullptr) != ERROR_SUCCESS) return false;
    const LONG result = RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
bool Exists(HKEY root, const wchar_t* path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}
DWORD SubkeyCount(HKEY root, const wchar_t* path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return MAXDWORD;
    DWORD count = 0;
    const LONG result = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &count,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS ? count : MAXDWORD;
}
} // namespace

int main() {
    using namespace desk::testing;
    const std::wstring fixture = L"Software\\MintDeskTests-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY root = nullptr;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, fixture.c_str(), 0, nullptr, 0,
        KEY_READ | KEY_WRITE | DELETE, nullptr, &root, nullptr) == ERROR_SUCCESS,
        "create isolated registry fixture");
    if (!root) return 1;
    const std::wstring executable = L"D:\\Program Files\\\u684c\u9762\u5361\u7247\\MintDesk.exe";
    const std::wstring otherExecutable = L"D:\\Moved\\MintDesk.exe";
    Check(Set(root, L"Unrelated\\command", nullptr, L"neighbor sentinel"), "write neighbor");
    Check(!IsDesktopMenuRegisteredAtKey(root, executable), "absent menu is disabled");
    Check(SetDesktopMenuAtKey(root, executable, false), "unregister absent menu is success");
    Check(RefreshDesktopMenuAtKey(root, executable, true), "refresh absent menu is success");
    Check(!Exists(root, L"MintDesk"), "refresh never enables absent registration");
    Check(SetDesktopMenuAtKey(root, executable, true), "register menu");
    Check(IsDesktopMenuRegisteredAtKey(root, executable), "registration matches executable");
    Check(!IsDesktopMenuRegisteredAtKey(root, otherExecutable), "moved executable is detected");
    Check(Read(root, L"MintDesk", L"MUIVerb") == L"\u6253\u5f00\u8584\u8377\u684c\u9762\u52a9\u624b",
        "stopped title is open");
    Check(Read(root, L"MintDesk", L"Icon") == L"\"" + executable + L"\",0",
        "icon path is quoted");
    Check(Read(root, L"MintDesk\\command", nullptr) ==
        L"\"" + executable + L"\" /toggle", "quoted Unicode toggle command");
    Check(!Exists(root, L"MintDesk\\shell"), "no nested submenu");
    Check(SubkeyCount(root, L"MintDesk") == 1, "only direct command is registered");
    Check(RefreshDesktopMenuAtKey(root, executable, true), "refresh running title");
    Check(Read(root, L"MintDesk", L"MUIVerb") == L"\u9000\u51fa\u8584\u8377\u684c\u9762\u52a9\u624b",
        "running title is exit");
    Check(IsDesktopMenuRegisteredAtKey(root, executable), "running title is enabled");
    Check(Read(root, L"MintDesk\\command", nullptr) ==
        L"\"" + executable + L"\" /toggle", "refresh leaves command intact");
    Check(RefreshDesktopMenuAtKey(root, executable, false), "refresh stopped title");
    Check(Read(root, L"MintDesk", L"MUIVerb") == L"\u6253\u5f00\u8584\u8377\u684c\u9762\u52a9\u624b",
        "stopped title returns to open");
    Check(!RefreshDesktopMenuAtKey(root, otherExecutable, true), "refresh does not adopt another executable");
    Check(SetDesktopMenuAtKey(root, executable, true), "registration is idempotent");
    Check(Set(root, L"MintDesk\\command", nullptr, executable + L" /toggle"),
        "replace command with unquoted fixture");
    Check(!IsDesktopMenuRegisteredAtKey(root, executable), "unquoted command is not enabled");
    Check(SetDesktopMenuAtKey(root, executable, true), "repair command");
    Check(Set(root, L"MintDesk", L"SubCommands", L""), "write legacy submenu value");
    Check(Set(root, L"MintDesk\\shell\\01show\\command", nullptr,
        L"\"" + executable + L"\" /show"), "write legacy show verb");
    Check(Set(root, L"MintDesk\\shell\\02close\\command", nullptr,
        L"\"" + executable + L"\" /exit"), "write legacy close verb");
    Check(Set(root, L"MintDesk\\shell\\02new\\command", nullptr,
        L"\"" + executable + L"\" /new-card"), "write legacy new-card verb");
    Check(!IsDesktopMenuRegisteredAtKey(root, executable), "legacy submenu is not direct registration");
    Check(SetDesktopMenuAtKey(root, executable, true), "upgrade legacy registration");
    Check(IsDesktopMenuRegisteredAtKey(root, executable), "upgraded registration matches");
    Check(!Exists(root, L"MintDesk\\shell\\02new"), "upgrade removes legacy new-card verb");
    Check(!Exists(root, L"MintDesk\\shell"), "upgrade removes whole legacy submenu");
    Check(SubkeyCount(root, L"MintDesk") == 1, "upgrade leaves only direct command");
    HKEY menuKey = nullptr;
    Check(RegOpenKeyExW(root, L"MintDesk", 0, KEY_READ, &menuKey) == ERROR_SUCCESS,
        "open menu for legacy value check");
    if (menuKey) {
        Check(RegQueryValueExW(menuKey, L"SubCommands", nullptr, nullptr, nullptr, nullptr) ==
            ERROR_FILE_NOT_FOUND, "upgrade removes submenu value");
        RegCloseKey(menuKey);
    }
    Check(Set(root, L"MintDesk\\shell\\02new\\command", nullptr,
        L"\"" + executable + L"\" /new-card"), "add stale extra verb");
    Check(!IsDesktopMenuRegisteredAtKey(root, executable), "extra stale verb invalidates registration");
    Check(SetDesktopMenuAtKey(root, executable, true), "remove stale extra verb");
    Check(SubkeyCount(root, L"MintDesk") == 1, "repair retains only direct command");
    Check(Set(root, L"MintDesk", L"LegacyDisable", L""), "hide incomplete fixture");
    Check(!IsDesktopMenuRegisteredAtKey(root, executable), "hidden registration is not enabled");
    Check(SetDesktopMenuAtKey(root, otherExecutable, true, true), "update moved executable while running");
    Check(IsDesktopMenuRegisteredAtKey(root, otherExecutable), "updated path is enabled");
    Check(Read(root, L"MintDesk", L"MUIVerb") == L"\u9000\u51fa\u8584\u8377\u684c\u9762\u52a9\u624b",
        "register running app directly offers exit");
    Check(SetDesktopMenuAtKey(root, otherExecutable, false), "unregister menu");
    Check(!Exists(root, L"MintDesk"), "unregister removes owned subtree");
    Check(RefreshDesktopMenuAtKey(root, otherExecutable, false), "refresh after unregister succeeds");
    Check(!Exists(root, L"MintDesk"), "stopped refresh never recreates disabled menu");
    Check(Read(root, L"Unrelated\\command", nullptr) == L"neighbor sentinel",
        "unrelated sibling survives toggling");
    Check(SetDesktopMenuAtKey(root, L"", true) == false, "empty path is rejected");
    Check(SetDesktopMenuAtKey(root, L"D:\\bad\"path.exe", true) == false, "quote in path is rejected");
    Check(SetDesktopMenuAtKey(root, std::wstring(L"D:\\bad\0path.exe", 15), true) == false,
        "embedded null in path is rejected");
    Check(!Exists(root, L"MintDesk"), "invalid input creates no registration");
    Check(!SetDesktopMenuAtKey(nullptr, executable, true), "invalid registry handle is rejected");
    Check(!RefreshDesktopMenuAtKey(nullptr, executable, true), "invalid refresh handle is rejected");
    RegCloseKey(root);
    Check(RegDeleteTreeW(HKEY_CURRENT_USER, fixture.c_str()) == ERROR_SUCCESS,
        "remove isolated registry fixture");
    if (!failures) std::puts("Desktop context menu tests passed.");
    return failures ? 1 : 0;
}
