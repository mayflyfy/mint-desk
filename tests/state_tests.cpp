#include "../src/state.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
bool Same(const desk::AppState& a, const desk::AppState& b) {
    if (a.nextId != b.nextId || a.opacity != b.opacity || a.cards.size() != b.cards.size()) return false;
    for (size_t i = 0; i < a.cards.size(); ++i) {
        const auto& x = a.cards[i];
        const auto& y = b.cards[i];
        if (x.id != y.id || x.title != y.title || x.monitor != y.monitor ||
            x.x != y.x || x.y != y.y || x.width != y.width || x.height != y.height ||
            x.layout != y.layout || x.locked != y.locked || x.collapsed != y.collapsed ||
            x.paths != y.paths) return false;
    }
    return true;
}
std::vector<unsigned char> Read(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(file, nullptr), read = 0;
    std::vector<unsigned char> data(size);
    if (!ReadFile(file, data.data(), size, &read, nullptr) || read != size) data.clear();
    CloseHandle(file);
    return data;
}
void Write(const std::wstring& path, const std::vector<unsigned char>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open fixture");
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    Check(WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
        written == data.size(), "write fixture");
    CloseHandle(file);
}
uint32_t Crc(const unsigned char* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void Put32(std::vector<unsigned char>& data, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}
void RefreshCrc(std::vector<unsigned char>& data) { Put32(data, 16, Crc(data.data() + 20, data.size() - 20)); }
void Append32(std::vector<unsigned char>& data, uint32_t value) {
    const size_t offset = data.size();
    data.resize(offset + 4);
    Put32(data, offset, value);
}
void AppendText(std::vector<unsigned char>& data, const std::wstring& value) {
    Append32(data, static_cast<uint32_t>(value.size()));
    for (wchar_t ch : value) {
        data.push_back(static_cast<unsigned char>(ch));
        data.push_back(static_cast<unsigned char>(ch >> 8));
    }
}
// Encode the original format independently so migration also protects path ordering.
std::vector<unsigned char> LegacyFixture(const desk::AppState& state) {
    std::vector<unsigned char> data{'D', 'S', 'K', 'C', 'R', 'D', '0', '1'};
    Append32(data, 1);
    Append32(data, 0);
    Append32(data, 0);
    Append32(data, state.nextId);
    Append32(data, state.opacity);
    Append32(data, static_cast<uint32_t>(state.cards.size()));
    for (const auto& card : state.cards) {
        Append32(data, card.id);
        AppendText(data, card.title);
        AppendText(data, card.monitor);
        Append32(data, static_cast<uint32_t>(card.x));
        Append32(data, static_cast<uint32_t>(card.y));
        Append32(data, static_cast<uint32_t>(card.width));
        Append32(data, static_cast<uint32_t>(card.height));
        Append32(data, static_cast<uint32_t>(card.layout));
        Append32(data, card.locked ? 1 : 0);
        Append32(data, static_cast<uint32_t>(card.paths.size()));
        for (const auto& path : card.paths) AppendText(data, path);
    }
    Put32(data, 12, static_cast<uint32_t>(data.size() - 20));
    RefreshCrc(data);
    return data;
}
} // namespace

int main() {
    using namespace desk;
    wchar_t temporary[MAX_PATH];
    Check(GetTempPathW(MAX_PATH, temporary) > 0, "temp path");
    const std::wstring directory = std::wstring(temporary) + L"MintDesk-state-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Check(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "isolated directory");
    const auto brandRoot=directory+L"\\brand-root",mint=brandRoot+L"\\MintDesk",old=brandRoot+L"\\DeskCards";
    Check(CreateDirectoryW(brandRoot.c_str(),nullptr)!=FALSE,"create isolated brand-directory fixture");
    Check(testing::DataDirectoryUnder(brandRoot)==mint,"new installs use MintDesk data directory");
    Check(CreateDirectoryW(old.c_str(),nullptr)!=FALSE,"create legacy recovery directory");
    const auto legacyJournal=old+L"\\hidden-items.dat";
    const std::vector<unsigned char> journalSentinel={'r','e','c','o','v','e','r','y'};Write(legacyJournal,journalSentinel);
    Check(testing::DataDirectoryUnder(brandRoot)==old&&Read(legacyJournal)==journalSentinel&&
        GetFileAttributesW((mint+L"\\hidden-items.dat").c_str())==INVALID_FILE_ATTRIBUTES,
        "rename reuses legacy recovery directory without copying or moving its journal");
    DeleteFileW(legacyJournal.c_str());RemoveDirectoryW(old.c_str());RemoveDirectoryW(mint.c_str());RemoveDirectoryW(brandRoot.c_str());
    const auto primary = directory + L"\\state.dat";
    const auto backup = directory + L"\\state.backup.dat";

    AppState original;
    original.nextId = 9;
    original.opacity = 201;
    CardState card;
    card.id = 8;
    card.title = L"\u5f00\u53d1\u5de5\u5177 \U0001F4BB";
    card.monitor = L"\\\\.\\DISPLAY2";
    card.x = -20; card.y = 76; card.width = 420; card.height = 350;
    card.locked = true; card.layout = Layout::List;
    card.paths = {L"D:\\\u9879\u76ee\\example.lnk", L"C:\\Program Files\\tool.exe"};
    original.cards.push_back(card);
    AppState loaded;
    Check(!testing::LoadStateFromDirectory(directory, loaded), "absent config");

    AppState legacy = original;
    legacy.nextId = 12;
    legacy.cards[0].paths.resize(1);
    CardState second = card;
    second.id = 9; second.title = L"Projects"; second.locked = false;
    second.layout = Layout::Grid; second.x = 410; second.y = -12;
    second.paths = {L"D:\\project\\folder"};
    legacy.cards.push_back(second);
    CardState third = card;
    third.id = 11; third.title = L"Tools"; third.monitor = L"\\\\.\\DISPLAY1";
    third.width = 280; third.height = 640;
    third.paths = {L"C:\\Program Files\\tool.exe"};
    legacy.cards.push_back(third);
    const auto legacyBytes = LegacyFixture(legacy);
    Write(primary, legacyBytes);
    legacy.opacity = 0;
    Check(testing::LoadStateFromDirectory(directory, loaded) && Same(loaded, legacy),
        "version 1 preserves all three cards and paths; defaults expanded with transparent background");
    Check(testing::SaveStateToDirectory(directory, loaded), "upgrade version 1 to version 2");
    const auto migratedBytes = Read(primary);
    Check(migratedBytes.size() > 12 && migratedBytes[8] == 2 && Read(backup) == legacyBytes,
        "upgrade writes version 2 and retains exact version 1 backup");
    Check(testing::LoadStateFromDirectory(directory, loaded) && Same(loaded, legacy),
        "migrated version 2 preserves every legacy field");

    original.cards[0].collapsed = true;
    Check(testing::SaveStateToDirectory(directory, original), "save first state");
    Check(testing::LoadStateFromDirectory(directory, loaded) && Same(loaded, original),
        "version 2 Unicode, collapsed flag and expanded height round trip");
    const auto valid = Read(primary);
    Check(!valid.empty(), "read persisted fixture");

    AppState newer = original;
    newer.cards[0].title = L"Updated";
    Check(testing::SaveStateToDirectory(directory, newer), "save replacement");
    auto corrupted = Read(primary);
    corrupted.back() ^= 0x40;
    Write(primary, corrupted);
    Check(testing::LoadStateFromDirectory(directory, loaded) && Same(loaded, original),
        "CRC failure recovers previous good backup");

    DeleteFileW(backup.c_str());
    const AppState sentinel = newer;
    loaded = sentinel;
    Check(!testing::LoadStateFromDirectory(directory, loaded) && Same(loaded, sentinel),
        "failed load preserves caller state");
    auto badVersion = valid;
    Put32(badVersion, 8, 3);
    Write(primary, badVersion);
    Check(!testing::LoadStateFromDirectory(directory, loaded), "unknown version rejected");
    auto hugeCount = valid;
    Put32(hugeCount, 28, 0xFFFFFFFFu);
    RefreshCrc(hugeCount);
    Write(primary, hugeCount);
    Check(!testing::LoadStateFromDirectory(directory, loaded), "bounded card count despite valid CRC");
    auto invalidOpacity = valid;
    Put32(invalidOpacity, 24, 256);
    RefreshCrc(invalidOpacity);
    Write(primary, invalidOpacity);
    Check(!testing::LoadStateFromDirectory(directory, loaded), "invalid opacity rejected");
    auto invalidCollapsed = valid;
    const size_t collapsedOffset = 68 + 2 * (original.cards[0].title.size() +
        original.cards[0].monitor.size());
    Put32(invalidCollapsed, collapsedOffset, 2);
    RefreshCrc(invalidCollapsed);
    Write(primary, invalidCollapsed);
    Check(!testing::LoadStateFromDirectory(directory, loaded), "invalid collapsed flag rejected");
    auto truncated = valid;
    truncated.resize(truncated.size() - 1);
    Write(primary, truncated);
    Check(!testing::LoadStateFromDirectory(directory, loaded), "truncated payload rejected");
    auto trailing = valid;
    trailing.push_back(0);
    Write(primary, trailing);
    Check(!testing::LoadStateFromDirectory(directory, loaded), "trailing bytes rejected");

    AppState invalid = original;
    invalid.cards.push_back(invalid.cards[0]);
    Write(primary, valid);
    Check(!testing::SaveStateToDirectory(directory, invalid) && Read(primary) == valid,
        "duplicate ID rejected before replacing disk state");
    invalid = original;
    invalid.cards[0].paths[0] = std::wstring(32768, L'x');
    Check(!testing::SaveStateToDirectory(directory, invalid), "oversize path rejected");
    invalid = original;
    invalid.cards[0].title.assign(1, static_cast<wchar_t>(0xD800));
    Check(!testing::SaveStateToDirectory(directory, invalid), "unpaired surrogate rejected");
    invalid = original;
    invalid.cards[0].height = 0;
    Check(!testing::SaveStateToDirectory(directory, invalid), "collapsed card still validates expanded height minimum");
    invalid.cards[0].height = 32769;
    Check(!testing::SaveStateToDirectory(directory, invalid), "collapsed card still validates expanded height maximum");

    std::vector<MonitorInfo> monitors = {
        {L"primary", RECT{0, 0, 1920, 1040}, 96},
        {L"left", RECT{-2560, 0, 0, 1400}, 144}
    };
    card.monitor = L"left"; card.x = 100; card.y = 200; card.width = 340; card.height = 420;
    auto rect = ResolveCardRect(card, monitors);
    Check(rect.left == -2410 && rect.top == 300 && rect.right - rect.left == 510 &&
        rect.bottom - rect.top == 630, "negative-origin monitor and 150 percent DPI");
    card.collapsed = true;
    rect = ResolveCardRect(card, monitors);
    Check(rect.left == -2410 && rect.top == 300 && rect.right - rect.left == 510 &&
        rect.bottom - rect.top == 48 && card.height == 420,
        "collapsed card resolves to 32 DIPs while preserving expanded height");
    card.y = 90000;
    rect = ResolveCardRect(card, monitors);
    Check(rect.bottom == 1400 && rect.top == 1352,
        "collapsed header remains reachable at monitor bottom");
    card.collapsed = false;
    card.monitor = L"disconnected"; card.x = 90000; card.y = 90000;
    rect = ResolveCardRect(card, monitors);
    Check(rect.left == 1580 && rect.top == 620 && rect.right == 1920 && rect.bottom == 1040,
        "unplugged monitor falls back and clamps entire card");
    card.width = 30000; card.height = 30000; card.x = -100; card.y = -100;
    rect = ResolveCardRect(card, monitors);
    Check(rect.left == 0 && rect.top == 0 && rect.right == 1920 && rect.bottom == 1040,
        "oversize card fits smaller work area");
    monitors = {{L"tiny", RECT{10, 20, 90, 80}, 96}};
    rect = ResolveCardRect(card, monitors);
    Check(rect.left == 10 && rect.top == 20 && rect.right == 90 && rect.bottom == 80,
        "tiny work area stays reachable");
    card.collapsed = true;
    monitors[0].work.bottom = 36;
    rect = ResolveCardRect(card, monitors);
    Check(rect.top == 20 && rect.bottom == 36, "collapsed header fits even a tiny work area");

    HWND captureWindow = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"MintDesk capture test",
        WS_POPUP, 100, 120, 360, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(captureWindow != nullptr, "create isolated native capture window");
    if (captureWindow) {
        CardState captured;
        captured.collapsed = true; captured.height = 777;
        captured.x = -999; captured.y = -999; captured.width = 1;
        CaptureCardRect(captured, captureWindow);
        Check(captured.height == 777 && captured.width > 1 && !captured.monitor.empty() &&
            captured.x != -999 && captured.y != -999,
            "capture updates collapsed position, width and monitor without losing expanded height");
        CardState expanded = captured;
        expanded.collapsed = false;
        CaptureCardRect(expanded, captureWindow);
        Check(expanded.height > 0 && expanded.height < 100 && expanded.x == captured.x &&
            expanded.y == captured.y && expanded.width == captured.width && expanded.monitor == captured.monitor,
            "expanded capture records actual native window height");
        DestroyWindow(captureWindow);
    }
    Check(!EnumerateMonitors().empty(), "native monitor enumeration");
    Check(!ExecutablePath().empty(), "native executable path");

    DeleteFileW(primary.c_str());
    DeleteFileW(backup.c_str());
    Check(RemoveDirectoryW(directory.c_str()) != FALSE, "isolated directory cleanup");
    if (!failures) std::puts("state tests passed");
    return failures ? 1 : 0;
}
