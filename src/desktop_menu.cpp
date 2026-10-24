#include "desktop_menu.h"
#include "state.h"

#include <shlobj.h>

namespace desk {
namespace {
constexpr wchar_t kShellPath[] = L"Software\\Classes\\DesktopBackground\\Shell";
constexpr wchar_t kMenuKey[] = L"MintDesk";
constexpr wchar_t kShowTitle[] = L"\u6253\u5f00\u8584\u8377\u684c\u9762\u52a9\u624b";
constexpr wchar_t kCloseTitle[] = L"\u9000\u51fa\u8584\u8377\u684c\u9762\u52a9\u624b";

class RegistryKey {
public:
    RegistryKey() = default;
    ~RegistryKey() { if (value_) RegCloseKey(value_); }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    HKEY get() const { return value_; }
    HKEY* address() { return &value_; }
private:
    HKEY value_ = nullptr;
};

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool StringEquals(HKEY key, const wchar_t* name, const std::wstring& expected) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 131072 ||
        bytes % sizeof(wchar_t) != 0) return false;
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(&value[0]), &bytes) != ERROR_SUCCESS || type != REG_SZ ||
        bytes != value.size() * sizeof(wchar_t) || value.back() != L'\0') return false;
    value.pop_back();
    return value == expected;
}

bool Open(HKEY parent, const wchar_t* path, REGSAM access, RegistryKey& key) {
    return RegOpenKeyExW(parent, path, 0, access, key.address()) == ERROR_SUCCESS;
}

bool Create(HKEY parent, const wchar_t* path, RegistryKey& key) {
    return RegCreateKeyExW(parent, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE | DELETE, nullptr, key.address(), nullptr) == ERROR_SUCCESS;
}

bool ValidExecutable(const std::wstring& executable) {
    if (executable.empty() || executable.size() > 32760) return false;
    for (wchar_t character : executable) {
        if (character < L' ' || character == L'"') return false;
    }
    return true;
}

std::wstring Command(const std::wstring& executable, const wchar_t* argument) {
    return L"\"" + executable + L"\" " + argument;
}

bool DeleteMenu(HKEY parent) {
    // RegDeleteTree removes only this named subtree, never its parent or siblings.
    const LONG result = RegDeleteTreeW(parent, kMenuKey);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND ||
        result == ERROR_PATH_NOT_FOUND;
}

bool WriteMenu(HKEY parent, const std::wstring& executable, bool running) {
    RegistryKey menu;
    if (!Create(parent, kMenuKey, menu)) return false;
    // Keep an incomplete registration out of Explorer's menu. Publish the title
    // and remove this marker only after the executable command is complete.
    if (!SetString(menu.get(), L"LegacyDisable", L"")) return false;
    // This subtree belongs to MintDesk. Rebuild it to remove obsolete verbs
    // such as the previous 02new entry when upgrading an existing install.
    const LONG removed = RegDeleteTreeW(menu.get(), L"shell");
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND &&
        removed != ERROR_PATH_NOT_FOUND) return false;
    const LONG removedSubcommands = RegDeleteValueW(menu.get(), L"SubCommands");
    if (removedSubcommands != ERROR_SUCCESS && removedSubcommands != ERROR_FILE_NOT_FOUND)
        return false;
    RegistryKey commandKey;
    const bool complete =
        Create(menu.get(), L"command", commandKey) &&
        SetString(commandKey.get(), nullptr, Command(executable, L"/toggle")) &&
        SetString(menu.get(), L"Icon", L"\"" + executable + L"\",0") &&
        SetString(menu.get(), L"MUIVerb", running ? kCloseTitle : kShowTitle);
    if (!complete) return false;
    return RegDeleteValueW(menu.get(), L"LegacyDisable") == ERROR_SUCCESS;
}

bool MenuMatches(HKEY parent, const std::wstring& executable) {
    if (!parent || !ValidExecutable(executable)) return false;
    RegistryKey menu, shell, command;
    if (!Open(parent, kMenuKey, KEY_READ, menu)) return false;
    // A failed/in-progress registration is deliberately not reported as enabled.
    if (RegQueryValueExW(menu.get(), L"LegacyDisable", nullptr, nullptr,
        nullptr, nullptr) != ERROR_FILE_NOT_FOUND) return false;
    if (RegQueryValueExW(menu.get(), L"SubCommands", nullptr, nullptr,
        nullptr, nullptr) != ERROR_FILE_NOT_FOUND ||
        Open(menu.get(), L"shell", KEY_READ, shell)) return false;
    DWORD subkeys = 0;
    if (RegQueryInfoKeyW(menu.get(), nullptr, nullptr, nullptr, &subkeys,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS ||
        subkeys != 1) return false;
    return (StringEquals(menu.get(), L"MUIVerb", kShowTitle) ||
        StringEquals(menu.get(), L"MUIVerb", kCloseTitle)) &&
        StringEquals(menu.get(), L"Icon", L"\"" + executable + L"\",0") &&
        Open(menu.get(), L"command", KEY_READ, command) &&
        StringEquals(command.get(), nullptr, Command(executable, L"/toggle"));
}

bool SetMenu(HKEY parent, const std::wstring& executable, bool enabled, bool running) {
    if (!parent) return false;
    if (!enabled) return DeleteMenu(parent);
    if (!ValidExecutable(executable)) return false;
    if (WriteMenu(parent, executable, running) && MenuMatches(parent, executable)) return true;
    // A partial menu cannot launch correctly. Remove only our own registration.
    DeleteMenu(parent);
    return false;
}

bool RefreshMenu(HKEY parent, const std::wstring& executable, bool running) {
    if (!parent || !ValidExecutable(executable)) return false;
    RegistryKey menu;
    const LONG result = RegOpenKeyExW(parent, kMenuKey, 0,
        KEY_READ | KEY_SET_VALUE, menu.address());
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS || !MenuMatches(parent, executable)) return false;
    return SetString(menu.get(), L"MUIVerb", running ? kCloseTitle : kShowTitle);
}
} // namespace

bool SetDesktopMenu(bool enabled) {
    try {
        RegistryKey shell;
        if (enabled) {
            if (!Create(HKEY_CURRENT_USER, kShellPath, shell)) return false;
        } else {
            const LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, kShellPath, 0,
                KEY_READ | KEY_WRITE | DELETE, shell.address());
            if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return true;
            if (result != ERROR_SUCCESS) return false;
        }
        const bool running = FindWindowW(L"MintDesk.Controller.1", L"MintDesk") != nullptr;
        if (!SetMenu(shell.get(), enabled ? ExecutablePath() : L"", enabled, running)) return false;
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    } catch (...) { return false; }
}

bool RefreshDesktopMenu(bool running) {
    try {
        RegistryKey shell;
        const LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, kShellPath, 0,
            KEY_READ | KEY_WRITE, shell.address());
        if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return true;
        if (result != ERROR_SUCCESS || !RefreshMenu(shell.get(), ExecutablePath(), running))
            return false;
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    } catch (...) { return false; }
}

bool IsDesktopMenuRegistered() {
    try {
        RegistryKey shell;
        return Open(HKEY_CURRENT_USER, kShellPath, KEY_READ, shell) &&
            MenuMatches(shell.get(), ExecutablePath());
    } catch (...) { return false; }
}

namespace testing {
bool SetDesktopMenuAtKey(HKEY shellParent, const std::wstring& executable, bool enabled,
    bool running) {
    try { return SetMenu(shellParent, executable, enabled, running); }
    catch (...) { return false; }
}
bool RefreshDesktopMenuAtKey(HKEY shellParent, const std::wstring& executable, bool running) {
    try { return RefreshMenu(shellParent, executable, running); }
    catch (...) { return false; }
}
bool IsDesktopMenuRegisteredAtKey(HKEY shellParent, const std::wstring& executable) {
    try { return MenuMatches(shellParent, executable); }
    catch (...) { return false; }
}
} // namespace testing
} // namespace desk
