#include "state.h"

#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace desk {
namespace {

constexpr uint32_t kVersion = 2;
constexpr size_t kHeaderSize = 20;
constexpr size_t kMaxFileSize = 16 * 1024 * 1024;
constexpr uint32_t kMaxCards = 256;
constexpr uint32_t kMaxPathsPerCard = 4096;
constexpr uint32_t kMaxTotalPaths = 20000;
constexpr uint32_t kMaxPathLength = 32767;
constexpr uint32_t kMaxLabelLength = 256;
constexpr wchar_t kStateFile[] = L"state.dat";
constexpr wchar_t kBackupFile[] = L"state.backup.dat";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunName[] = L"MintDesk";
const unsigned char kMagic[8] = {'D', 'S', 'K', 'C', 'R', 'D', '0', '1'};
SRWLOCK gStateLock = SRWLOCK_INIT;

struct Lock {
    explicit Lock(bool write) : write_(write) {
        if (write_) AcquireSRWLockExclusive(&gStateLock);
        else AcquireSRWLockShared(&gStateLock);
    }
    ~Lock() {
        if (write_) ReleaseSRWLockExclusive(&gStateLock);
        else ReleaseSRWLockShared(&gStateLock);
    }
    bool write_;
};

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
    void close() {
        if (valid()) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
private:
    HANDLE value_;
};

bool ValidText(const std::wstring& value, uint32_t limit) {
    if (value.size() > limit) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const uint16_t ch = static_cast<uint16_t>(value[i]);
        if (!ch) return false;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i == value.size()) return false;
            const uint16_t low = static_cast<uint16_t>(value[i]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

bool Validate(const AppState& state, uint32_t version = kVersion) {
    if (!state.nextId || state.cards.size() > kMaxCards) return false;
    std::set<uint32_t> ids;
    size_t totalPaths = 0;
    size_t encodedSize = kHeaderSize + 12;
    for (const auto& card : state.cards) {
        if (!card.id || card.id >= state.nextId || !ids.insert(card.id).second ||
            !ValidText(card.title, kMaxLabelLength) ||
            !ValidText(card.monitor, kMaxLabelLength) ||
            card.width < 1 || card.height < 1 ||
            card.width > 32768 || card.height > 32768 ||
            card.x < -1000000 || card.x > 1000000 ||
            card.y < -1000000 || card.y > 1000000 ||
            (card.layout != Layout::Grid && card.layout != Layout::List) ||
            card.paths.size() > kMaxPathsPerCard) return false;
        totalPaths += card.paths.size();
        if (totalPaths > kMaxTotalPaths) return false;
        encodedSize += (version >= 2 ? 44 : 40) + 2 * (card.title.size() + card.monitor.size());
        for (const auto& path : card.paths) {
            if (path.empty() || !ValidText(path, kMaxPathLength)) return false;
            encodedSize += 4 + 2 * path.size();
            if (encodedSize > kMaxFileSize) return false;
        }
        if (encodedSize > kMaxFileSize) return false;
    }
    return true;
}

void Put32(std::vector<unsigned char>& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xFF));
}

void PutText(std::vector<unsigned char>& out, const std::wstring& text) {
    Put32(out, static_cast<uint32_t>(text.size()));
    for (wchar_t ch : text) {
        out.push_back(static_cast<unsigned char>(ch & 0xFF));
        out.push_back(static_cast<unsigned char>((ch >> 8) & 0xFF));
    }
}

uint32_t Checksum(const unsigned char* bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

class Reader {
public:
    Reader(const unsigned char* data, size_t size) : data_(data), size_(size) {}
    bool u32(uint32_t& value) {
        if (size_ - offset_ < 4) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<uint32_t>(data_[offset_++]) << shift;
        return true;
    }
    bool integer(int& value) {
        uint32_t bits;
        if (!u32(bits)) return false;
        int32_t signedValue;
        std::memcpy(&signedValue, &bits, sizeof(bits));
        value = signedValue;
        return true;
    }
    bool text(std::wstring& value, uint32_t limit) {
        uint32_t count;
        if (!u32(count) || count > limit || count > (size_ - offset_) / 2) return false;
        value.clear();
        value.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t ch = static_cast<uint16_t>(data_[offset_]) |
                (static_cast<uint16_t>(data_[offset_ + 1]) << 8);
            value.push_back(static_cast<wchar_t>(ch));
            offset_ += 2;
        }
        return ValidText(value, limit);
    }
    bool done() const { return offset_ == size_; }
private:
    const unsigned char* data_;
    size_t size_;
    size_t offset_ = 0;
};

bool Encode(const AppState& state, std::vector<unsigned char>& output) {
    if (!Validate(state)) return false;
    std::vector<unsigned char> payload;
    Put32(payload, state.nextId);
    Put32(payload, state.opacity);
    Put32(payload, static_cast<uint32_t>(state.cards.size()));
    for (const auto& card : state.cards) {
        Put32(payload, card.id);
        PutText(payload, card.title);
        PutText(payload, card.monitor);
        Put32(payload, static_cast<uint32_t>(card.x));
        Put32(payload, static_cast<uint32_t>(card.y));
        Put32(payload, static_cast<uint32_t>(card.width));
        Put32(payload, static_cast<uint32_t>(card.height));
        Put32(payload, static_cast<uint32_t>(card.layout));
        Put32(payload, card.locked ? 1 : 0);
        Put32(payload, card.collapsed ? 1 : 0);
        Put32(payload, static_cast<uint32_t>(card.paths.size()));
        for (const auto& path : card.paths) PutText(payload, path);
        if (payload.size() > kMaxFileSize - kHeaderSize) return false;
    }
    output.assign(kMagic, kMagic + sizeof(kMagic));
    Put32(output, kVersion);
    Put32(output, static_cast<uint32_t>(payload.size()));
    Put32(output, Checksum(payload.data(), payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());
    return true;
}

bool Decode(const std::vector<unsigned char>& bytes, AppState& state) {
    if (bytes.size() < kHeaderSize || bytes.size() > kMaxFileSize ||
        std::memcmp(bytes.data(), kMagic, sizeof(kMagic))) return false;
    Reader header(bytes.data() + 8, 12);
    uint32_t version, size, checksum;
    if (!header.u32(version) || !header.u32(size) || !header.u32(checksum) ||
        (version != 1 && version != kVersion) || size != bytes.size() - kHeaderSize ||
        checksum != Checksum(bytes.data() + kHeaderSize, size)) return false;
    Reader reader(bytes.data() + kHeaderSize, size);
    AppState parsed;
    uint32_t opacity, count;
    if (!reader.u32(parsed.nextId) || !reader.u32(opacity) || opacity > 255 ||
        !reader.u32(count) || count > kMaxCards) return false;
    // Version 1 dimmed the entire window. Its value is not a background preference.
    parsed.opacity = version == 1 ? 0 : static_cast<BYTE>(opacity);
    parsed.cards.reserve(count);
    uint32_t totalPaths = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CardState card;
        uint32_t layout, locked, collapsed = 0, pathCount;
        if (!reader.u32(card.id) || !reader.text(card.title, kMaxLabelLength) ||
            !reader.text(card.monitor, kMaxLabelLength) || !reader.integer(card.x) ||
            !reader.integer(card.y) || !reader.integer(card.width) ||
            !reader.integer(card.height) || !reader.u32(layout) || layout > 1 ||
            !reader.u32(locked) || locked > 1 ||
            (version >= 2 && (!reader.u32(collapsed) || collapsed > 1)) ||
            !reader.u32(pathCount) ||
            pathCount > kMaxPathsPerCard || pathCount > kMaxTotalPaths - totalPaths)
            return false;
        card.layout = static_cast<Layout>(layout);
        card.locked = locked != 0;
        card.collapsed = collapsed != 0;
        totalPaths += pathCount;
        card.paths.reserve(pathCount);
        for (uint32_t j = 0; j < pathCount; ++j) {
            std::wstring path;
            if (!reader.text(path, kMaxPathLength)) return false;
            card.paths.push_back(std::move(path));
        }
        parsed.cards.push_back(std::move(card));
    }
    if (!reader.done() || !Validate(parsed, version)) return false;
    state = std::move(parsed);
    return true;
}

std::wstring Join(const std::wstring& directory, const wchar_t* name) {
    if (directory.empty()) return {};
    return directory + ((directory.back() == L'\\' || directory.back() == L'/') ? L"" : L"\\") + name;
}

bool EnsureDirectory(const std::wstring& directory) {
    if (directory.empty()) return false;
    if (CreateDirectoryW(directory.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) return false;
    LARGE_INTEGER length;
    if (!GetFileSizeEx(file.get(), &length) || length.QuadPart < static_cast<LONGLONG>(kHeaderSize) ||
        length.QuadPart > static_cast<LONGLONG>(kMaxFileSize)) return false;
    bytes.resize(static_cast<size_t>(length.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(file.get(), bytes.data() + offset,
            static_cast<DWORD>(bytes.size() - offset), &read, nullptr) || !read) return false;
        offset += read;
    }
    return true;
}

bool AtomicWrite(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    std::wstring temporary;
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt) + L".tmp";
        raw = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (raw != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    Handle file(raw);
    if (!file.valid()) return false;
    bool success = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + offset,
            static_cast<DWORD>(bytes.size() - offset), &written, nullptr) || !written) {
            success = false;
            break;
        }
        offset += written;
    }
    if (success) success = FlushFileBuffers(file.get()) != FALSE;
    file.close();
    if (success) success = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}

bool LoadDirectory(const std::wstring& directory, AppState& state) {
    if (directory.empty()) return false;
    std::vector<unsigned char> bytes;
    if (ReadFileBytes(Join(directory, kStateFile), bytes) && Decode(bytes, state)) return true;
    return ReadFileBytes(Join(directory, kBackupFile), bytes) && Decode(bytes, state);
}

bool SaveDirectory(const std::wstring& directory, const AppState& state) {
    std::vector<unsigned char> bytes;
    if (!Encode(state, bytes) || !EnsureDirectory(directory)) return false;
    std::vector<unsigned char> previous;
    AppState checked;
    const std::wstring primary = Join(directory, kStateFile);
    const std::wstring backup = Join(directory, kBackupFile);
    if (ReadFileBytes(primary, previous) && Decode(previous, checked)) {
        if (!AtomicWrite(backup, previous)) return false;
    } else if (!(ReadFileBytes(backup, previous) && Decode(previous, checked))) {
        // On the first save, both copies are valid before publishing the primary.
        if (!AtomicWrite(backup, bytes)) return false;
    }
    return AtomicWrite(primary, bytes);
}

UINT MonitorDpi(HMONITOR monitor) {
    using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    // Keep this system DLL loaded so cached function pointers remain valid.
    static HMODULE shcore = []() -> HMODULE {
        wchar_t system[MAX_PATH];
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH) return nullptr;
        const auto path = Join(system, L"shcore.dll");
        return LoadLibraryW(path.c_str());
    }();
    static GetDpiForMonitorFn getDpi = []() -> GetDpiForMonitorFn {
        if (!shcore) return nullptr;
        const FARPROC address = GetProcAddress(shcore, "GetDpiForMonitor");
        GetDpiForMonitorFn function = nullptr;
        static_assert(sizeof(function) == sizeof(address), "Windows function pointer size");
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }();
    UINT x = 96, y = 96;
    if (getDpi && SUCCEEDED(getDpi(monitor, 0, &x, &y)) && x >= 48 && x <= 768) return x;
    return 96;
}

BOOL CALLBACK MonitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) try {
        auto& list = *reinterpret_cast<std::vector<MonitorInfo>*>(data);
        MonitorInfo item{info.szDevice, info.rcWork, MonitorDpi(monitor)};
        // The first item is the primary monitor: it is the fallback after unplugging.
        if (info.dwFlags & MONITORINFOF_PRIMARY) list.insert(list.begin(), std::move(item));
        else list.push_back(std::move(item));
    } catch (...) { return FALSE; }
    return TRUE;
}

int DipToPixels(int value, UINT dpi) {
    const int64_t scaled = static_cast<int64_t>(value) * dpi;
    const int64_t rounded = (scaled + (scaled >= 0 ? 48 : -48)) / 96;
    return static_cast<int>(std::max<int64_t>(std::numeric_limits<int>::min(),
        std::min<int64_t>(std::numeric_limits<int>::max(), rounded)));
}

int PixelsToDip(int value, UINT dpi) {
    const int64_t scaled = static_cast<int64_t>(value) * 96;
    return static_cast<int>((scaled + (scaled >= 0 ? dpi / 2 : -static_cast<int>(dpi / 2))) / dpi);
}

std::wstring AutostartCommand() {
    const auto executable = ExecutablePath();
    if (executable.empty()) return {};
    return L"\"" + executable + L"\" /background";
}

} // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> result;
    EnumDisplayMonitors(nullptr, nullptr, MonitorCallback, reinterpret_cast<LPARAM>(&result));
    if (result.empty()) {
        RECT work{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            work = RECT{0, 0, std::max(1, GetSystemMetrics(SM_CXSCREEN)),
                std::max(1, GetSystemMetrics(SM_CYSCREEN))};
        result.push_back(MonitorInfo{L"", work, 96});
    }
    return result;
}

RECT ResolveCardRect(const CardState& card, const std::vector<MonitorInfo>& monitors) {
    MonitorInfo fallback{L"", RECT{0, 0, 1920, 1080}, 96};
    const MonitorInfo* monitor = monitors.empty() ? &fallback : &monitors.front();
    for (const auto& candidate : monitors) {
        if (candidate.device == card.monitor) { monitor = &candidate; break; }
    }
    const UINT dpi = monitor->dpi >= 48 && monitor->dpi <= 768 ? monitor->dpi : 96;
    const int availableWidth = std::max(1L, monitor->work.right - monitor->work.left);
    const int availableHeight = std::max(1L, monitor->work.bottom - monitor->work.top);
    const int width = std::min(availableWidth, std::max(DipToPixels(160, dpi), DipToPixels(card.width, dpi)));
    const int height = card.collapsed
        ? std::min(availableHeight, DipToPixels(32, dpi))
        : std::min(availableHeight, std::max(DipToPixels(100, dpi), DipToPixels(card.height, dpi)));
    const int x = std::max(0, std::min(availableWidth - width, DipToPixels(card.x, dpi)));
    const int y = std::max(0, std::min(availableHeight - height, DipToPixels(card.y, dpi)));
    return RECT{monitor->work.left + x, monitor->work.top + y,
        monitor->work.left + x + width, monitor->work.top + y + height};
}

void CaptureCardRect(CardState& card, HWND window) {
    RECT rect;
    if (!GetWindowRect(window, &rect)) return;
    const HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;
    const UINT dpi = MonitorDpi(monitor);
    card.monitor = info.szDevice;
    card.x = PixelsToDip(rect.left - info.rcWork.left, dpi);
    card.y = PixelsToDip(rect.top - info.rcWork.top, dpi);
    card.width = std::max(1, PixelsToDip(rect.right - rect.left, dpi));
    if (!card.collapsed)
        card.height = std::max(1, PixelsToDip(rect.bottom - rect.top, dpi));
}

std::wstring DataDirectoryUnder(const std::wstring& local) {
    // Existing recovery journals can refer to payloads in this directory.
    // Keep that location in place rather than copying or renaming user data.
    const auto legacy = Join(local, L"DeskCards");
    const DWORD legacyAttributes = GetFileAttributesW(legacy.c_str());
    const auto directory = legacyAttributes != INVALID_FILE_ATTRIBUTES &&
        (legacyAttributes & FILE_ATTRIBUTE_DIRECTORY) ? legacy : Join(local, L"MintDesk");
    return EnsureDirectory(directory) ? directory : std::wstring{};
}

std::wstring DataDirectory() {
    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
        nullptr, SHGFP_TYPE_CURRENT, local))) return {};
    return DataDirectoryUnder(local);
}

bool LoadState(AppState& state) {
    Lock lock(false);
    try { return LoadDirectory(DataDirectory(), state); }
    catch (...) { return false; }
}

bool SaveState(const AppState& state) {
    Lock lock(true);
    try { return SaveDirectory(DataDirectory(), state); }
    catch (...) { return false; }
}

std::wstring ExecutablePath() {
    std::wstring path(512, L'\0');
    for (; path.size() <= 32768; path.resize(path.size() * 2)) {
        const DWORD length = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
        if (!length) return {};
        if (length < path.size()) { path.resize(length); return path; }
    }
    return {};
}

bool SetAutostart(bool enabled) {
    HKEY key = nullptr;
    const LONG opened = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) return false;
    LONG result;
    if (enabled) {
        const auto command = AutostartCommand();
        result = command.empty() ? ERROR_INVALID_DATA : RegSetValueExW(key, kRunName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG result = RegQueryValueExW(key, kRunName, nullptr, &type, nullptr, &size);
    std::wstring value;
    if (result == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(wchar_t) &&
        size <= 65536 && size % sizeof(wchar_t) == 0) {
        value.resize(size / sizeof(wchar_t));
        result = RegQueryValueExW(key, kRunName, nullptr, &type,
            reinterpret_cast<BYTE*>(&value[0]), &size);
        if (result == ERROR_SUCCESS && value.back() == L'\0') value.pop_back();
        else value.clear();
    }
    RegCloseKey(key);
    return !value.empty() && value == AutostartCommand();
}

namespace testing {
std::wstring DataDirectoryUnder(const std::wstring& local) { return desk::DataDirectoryUnder(local); }
bool LoadStateFromDirectory(const std::wstring& directory, AppState& state) {
    Lock lock(false);
    try { return LoadDirectory(directory, state); }
    catch (...) { return false; }
}
bool SaveStateToDirectory(const std::wstring& directory, const AppState& state) {
    Lock lock(true);
    try { return SaveDirectory(directory, state); }
    catch (...) { return false; }
}
} // namespace testing
} // namespace desk
