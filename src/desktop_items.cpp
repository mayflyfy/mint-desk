#include "desktop_items.h"
#include "state.h"
#include "file_helper.h"
#include "hidden_items.h"

#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace desk {
namespace {

constexpr size_t kMaxJournal = 16 * 1024 * 1024;
constexpr uint32_t kMaxRecords = 20000;
constexpr uint32_t kMaxPath = 32767;
constexpr unsigned char kMagic[8] = {'D','S','K','M','O','V','0','1'};
enum class Phase : uint32_t { Moving = 1, Managed = 2, Restoring = 3, Alias = 4 };
struct Record {
    std::wstring reference, actual, target;
    Phase phase = Phase::Alias;
};
struct State {
    std::wstring directory, desktop, publicDesktop, storage;
    std::vector<Record> records;
    bool ready = false;
    bool injected = false;
    bool permissionRequired = false;
} g;
SRWLOCK gLock = SRWLOCK_INIT;
struct Lock {
    bool exclusive;
    explicit Lock(bool value) : exclusive(value) {
        if (exclusive) AcquireSRWLockExclusive(&gLock); else AcquireSRWLockShared(&gLock);
    }
    ~Lock() {
        if (exclusive) ReleaseSRWLockExclusive(&gLock); else ReleaseSRWLockShared(&gLock);
    }
};
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool Prefix(const std::wstring& parent, const std::wstring& path) {
    return !parent.empty() && path.size() > parent.size() && path[parent.size()] == L'\\' &&
        CompareStringOrdinal(parent.c_str(), static_cast<int>(parent.size()),
            path.c_str(), static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL;
}
std::wstring Join(const std::wstring& a, const std::wstring& b) {
    return a + (a.empty() || a.back() == L'\\' ? L"" : L"\\") + b;
}
std::wstring Normalize(const std::wstring& path) {
    if (path.empty() || path.size() > kMaxPath || path.find(L'\0') != std::wstring::npos) return {};
    // Only absolute filesystem paths may acquire desktop ownership.
    if (!((path.size() > 2 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
          (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\'))) return {};
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed || needed > kMaxPath) return {};
    std::wstring out(needed, L'\0');
    DWORD count = GetFullPathNameW(path.c_str(), needed, &out[0], nullptr);
    if (!count || count >= needed) return {};
    out.resize(count);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash == 2 ? 3 : slash);
}
std::wstring Name(const std::wstring& path) { return path.substr(path.find_last_of(L'\\') + 1); }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool Directory(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
bool DirectDesktop(const std::wstring& path) {
    const auto parent = Parent(path);
    return (!g.desktop.empty() && Equal(parent, g.desktop)) ||
        (!g.publicDesktop.empty() && Equal(parent, g.publicDesktop));
}
bool RestoreWritable(const std::wstring& source) {
    const auto parent = Parent(source);
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    for (unsigned i = 0; i < 16; ++i) {
        const auto probe = Join(parent, L".MintDesk-restore-probe-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i));
        {
            // DELETE_ON_CLOSE removes only this newly-created probe, including
            // on process termination. No existing source or user file is touched.
            Handle file(CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
            if (file.value == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
                return false;
            }
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
        // A directory additionally needs FILE_ADD_SUBDIRECTORY. Remove only
        // the empty probe directory we just created successfully ourselves.
        const auto folderProbe = probe + L"-dir";
        if (!CreateDirectoryW(folderProbe.c_str(), nullptr)) return false;
        return RemoveDirectoryW(folderProbe.c_str()) != FALSE;
    }
    return false;
}
DWORD RenameDesktopFile(const std::wstring& from,const std::wstring& to) {
    const DWORD attributes=GetFileAttributesW(from.c_str());
    DWORD error=ERROR_SUCCESS;
    bool usedHelper=false;
    if(!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)) {
        error=GetLastError();
        if(!g.injected&&error==ERROR_ACCESS_DENIED&&FileHelperRunning()) {usedHelper=true;error=PrivilegedDesktopMove(from,to);}
    }
    if(error==ERROR_SUCCESS&&!usedHelper) {
        // Moving across folders is a removal and an addition. A rename event
        // can reach subscribers promptly while the Desktop keeps its old item.
        const bool directory=attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY);
        SHChangeNotify(directory?SHCNE_RMDIR:SHCNE_DELETE,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,from.c_str(),nullptr);
        SHChangeNotify(directory?SHCNE_MKDIR:SHCNE_CREATE,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,to.c_str(),nullptr);
    }
    return error;
}
uint32_t Crc(const unsigned char* data, size_t size) {
    uint32_t result = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        result ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ (0xEDB88320u & (0u - (result & 1u)));
    }
    return ~result;
}
void Put32(std::vector<unsigned char>& data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void PutText(std::vector<unsigned char>& data, const std::wstring& value) {
    Put32(data, static_cast<uint32_t>(value.size()));
    for (wchar_t c : value) { data.push_back(static_cast<unsigned char>(c)); data.push_back(static_cast<unsigned char>(c >> 8)); }
}
struct Reader {
    const std::vector<unsigned char>& bytes;
    size_t at = 0;
    bool word(uint32_t& out) {
        if (at > bytes.size() || bytes.size() - at < 4) return false;
        out = 0;
        for (unsigned i = 0; i < 4; ++i) out |= static_cast<uint32_t>(bytes[at++]) << (8 * i);
        return true;
    }
    bool text(std::wstring& out) {
        uint32_t size;
        if (!word(size) || !size || size > kMaxPath || size > (bytes.size() - at) / 2) return false;
        out.clear(); out.reserve(size);
        for (uint32_t i = 0; i < size; ++i) {
            wchar_t c = static_cast<wchar_t>(bytes[at] | (bytes[at + 1] << 8)); at += 2;
            if (!c) return false;
            out.push_back(c);
        }
        return !Normalize(out).empty();
    }
};
struct PathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
bool ReadJournal(const std::wstring& path, std::vector<Record>& records) {
    Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.value == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.value, &size) || size.QuadPart < 24 || size.QuadPart > static_cast<LONGLONG>(kMaxJournal)) return false;
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(h.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size() ||
        std::memcmp(bytes.data(), kMagic, 8)) return false;
    Reader reader{bytes, 8};
    uint32_t version, payload, checksum, count;
    if (!reader.word(version) || version != 1 || !reader.word(payload) || payload != bytes.size() - 20 ||
        !reader.word(checksum) || checksum != Crc(bytes.data() + 20, bytes.size() - 20) ||
        !reader.word(count) || count > kMaxRecords) return false;
    std::vector<Record> parsed;
    std::set<std::wstring, PathLess> references;
    parsed.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Record record; uint32_t phase;
        if (!reader.word(phase) || phase < 1 || phase > 4 || !reader.text(record.reference) ||
            !reader.text(record.actual) || !reader.text(record.target)) return false;
        record.phase = static_cast<Phase>(phase);
        if (!references.insert(record.reference).second) return false;
        parsed.push_back(std::move(record));
    }
    if (reader.at != bytes.size()) return false;
    records = std::move(parsed);
    return true;
}
bool AtomicWrite(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    const auto temp = path + L".tmp";
    {
        Handle h(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (h.value == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!WriteFile(h.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size() || !FlushFileBuffers(h.value)) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
bool Persist() {
    if (g.directory.empty() || g.records.size() > kMaxRecords) return false;
    std::vector<unsigned char> payload;
    Put32(payload, static_cast<uint32_t>(g.records.size()));
    for (const auto& record : g.records) {
        Put32(payload, static_cast<uint32_t>(record.phase));
        PutText(payload, record.reference); PutText(payload, record.actual); PutText(payload, record.target);
        if (payload.size() > kMaxJournal - 20) return false;
    }
    std::vector<unsigned char> bytes(kMagic, kMagic + 8);
    Put32(bytes, 1); Put32(bytes, static_cast<uint32_t>(payload.size())); Put32(bytes, Crc(payload.data(), payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    // Both copies contain the intent before a user file can move. A crash after
    // either subsequent phase update can be recovered from either journal.
    return AtomicWrite(Join(g.directory, L"desktop-items.dat"), bytes) &&
        AtomicWrite(Join(g.directory, L"desktop-items.backup.dat"), bytes);
}
uint64_t Hash(const std::wstring& text) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : text) { hash ^= static_cast<uint16_t>(c); hash *= 1099511628211ull; }
    return hash;
}
std::wstring Volume(const std::wstring& path) {
    wchar_t mount[32768]{}, volume[64]{};
    if (!GetVolumePathNameW(path.c_str(), mount, 32768)) return {};
    if (GetVolumeNameForVolumeMountPointW(mount, volume, 64)) return volume;
    return mount;
}
std::wstring StorageFor(const std::wstring& original) {
    if (g.injected) return Directory(g.storage) ? g.storage : std::wstring{};
    const auto parent = Parent(original);
    const auto volume = Volume(parent);
    if (!volume.empty() && Equal(volume, Volume(g.directory))) {
        const auto storage = Join(g.directory, L"Managed");
        return Directory(storage) ? storage : std::wstring{};
    }
    // A redirected desktop on another volume must keep its managed folder on
    // that volume. Its sibling folder does not appear as a desktop icon.
    std::wstring identity;
    Handle token(nullptr);
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        token.value = rawToken;
        DWORD needed = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &needed);
        std::vector<unsigned char> user(needed);
        if (needed && GetTokenInformation(token.value, TokenUser, user.data(), needed, &needed)) {
            const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
            const auto* bytes = static_cast<const unsigned char*>(sid);
            for (DWORD i = 0; i < GetLengthSid(sid); ++i) identity.push_back(bytes[i]);
        }
    }
    if (identity.empty()) return {};
    const auto storage = Join(Parent(parent), L".MintDesk-Managed-" + std::to_wstring(Hash(identity)));
    if (!Directory(storage)) return {};
    DWORD attributes = GetFileAttributesW(storage.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(storage.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
    return storage;
}
std::wstring UniqueRestore(const std::wstring& requested) {
    if (!Exists(requested)) return requested;
    const auto parent = Parent(requested), name = Name(requested);
    const auto dot = name.find_last_of(L'.');
    const bool extension = dot != std::wstring::npos && dot > 0 && !(GetFileAttributesW(requested.c_str()) & FILE_ATTRIBUTE_DIRECTORY);
    const auto stem = extension ? name.substr(0, dot) : name;
    const auto suffix = extension ? name.substr(dot) : std::wstring{};
    for (unsigned i = 1; i < 100000; ++i) {
        const auto candidate = Join(parent, stem + L" (MintDesk " + std::to_wstring(i) + L")" + suffix);
        if (!Exists(candidate)) return candidate;
    }
    return {};
}
bool Reconcile(Record& record) {
    const bool actual = Exists(record.actual), target = Exists(record.target);
    if (record.phase == Phase::Moving) {
        // The same atomic rename cannot leave both names present. A second
        // file may have appeared at the reserved destination before a failed
        // move was recorded, so neither file can safely be assumed ours.
        if (actual && target) return false;
        if (actual) { record.phase = Phase::Managed; return true; }
        if (target) { record.actual = record.target; record.phase = Phase::Alias; return true; }
        return false;
    }
    if (record.phase == Phase::Restoring) {
        if (!actual && target) { record.actual = record.target; record.phase = Phase::Alias; return true; }
        if (actual) { record.phase = Phase::Managed; return true; }
        return false;
    }
    if (record.phase == Phase::Alias && !actual && DirectDesktop(record.reference) && Exists(record.reference)) {
        // An explicit elevated repair may return a recovered public entry to
        // its original Desktop path. Follow that external move in metadata;
        // reconciliation never moves or deletes either filesystem entry.
        record.actual = record.reference;
        record.target = record.reference;
        return true;
    }
    return actual;
}
const Record* FindResolvedRecord(const std::wstring& path) {
    const Record* best = nullptr;
    for (const auto& record : g.records) {
        if (Equal(record.reference, path)) return &record;
        if (Prefix(record.reference, path) && (!best || record.reference.size() > best->reference.size())) best = &record;
    }
    return best;
}
std::wstring ActualFor(const Record& record) {
    if (record.phase == Phase::Moving && (!Exists(record.actual) || Exists(record.target))) return record.target;
    if (record.phase == Phase::Restoring && !Exists(record.actual) && Exists(record.target)) return record.target;
    return record.actual;
}
bool Restore(Record& record) {
    if(!g.injected&&FileHelperOperationPending())return false;
    // An alias is an ordinary reference to a file already returned to Desktop.
    // The user may subsequently move/delete it; we hold no file to restore.
    if (!Reconcile(record)) return record.phase == Phase::Alias;
    if (record.phase == Phase::Alias) return true;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto target = UniqueRestore(record.target);
        if (target.empty()) return false;
        record.target = target; record.phase = Phase::Restoring;
        if (!Persist()) return false;
        const DWORD error=RenameDesktopFile(record.actual,target);
        if (error==ERROR_SUCCESS) {
            record.actual = target; record.phase = Phase::Alias;
            return Persist();
        }
        if(error==ERROR_IO_INCOMPLETE)return false;
        record.phase = Phase::Managed;
        if(error==ERROR_ACCESS_DENIED&&!g.injected&&DirectDesktop(target))g.permissionRequired=true;
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
    }
    return false;
}
bool Initialize(const std::wstring& directory, const std::wstring& desktop,
    const std::wstring& publicDesktop, const std::wstring& storage, bool injected) {
    g = {};
    g.directory = Normalize(directory); g.desktop = Normalize(desktop);
    g.publicDesktop = Normalize(publicDesktop); g.storage = Normalize(storage); g.injected = injected;
    if (!Directory(g.directory) || (g.desktop.empty() && g.publicDesktop.empty())) return false;
    const auto primary = Join(g.directory, L"desktop-items.dat"), backup = Join(g.directory, L"desktop-items.backup.dat");
    if (!ReadJournal(primary, g.records) && !ReadJournal(backup, g.records)) {
        if (Exists(primary) || Exists(backup)) return false;
    }
    bool allPresent = true;
    for (auto& record : g.records)
        if (!Reconcile(record) && record.phase != Phase::Alias) allPresent = false;
    g.ready = Persist();
    return g.ready && allPresent;
}

} // namespace

bool InitializeDesktopItems() {
    Lock lock(true);
    try {
        if(!WaitForFileMovesIdle())return false;
        PWSTR user = nullptr, common = nullptr;
        SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &user);
        SHGetKnownFolderPath(FOLDERID_PublicDesktop, KF_FLAG_DEFAULT, nullptr, &common);
        const std::wstring desktop = user ? user : L"", publicDesktop = common ? common : L"";
        CoTaskMemFree(user); CoTaskMemFree(common);
        const bool legacy=Initialize(DataDirectory(), desktop, publicDesktop, {}, false);
        return InitializeHiddenItems()&&legacy;
    } catch (...) { g.ready = false; return false; }
}
DesktopManageResult ManageDesktopItem(const std::wstring& reference) {
    Lock lock(true);
    try {
        if(!g.injected&&FileHelperOperationPending())return DesktopManageResult::Failed;
        if(!g.injected) {
            if(!g.ready)return DesktopManageResult::Failed;
            const auto normalized=Normalize(reference);const auto* prior=FindResolvedRecord(normalized);
            return HideDesktopItem(prior?ActualFor(*prior)+normalized.substr(prior->reference.size()):reference);
        }
        const auto path = Normalize(reference);
        if (Equal(path, Normalize(ExecutablePath()))) return DesktopManageResult::Reference;
        auto found = std::find_if(g.records.begin(), g.records.end(), [&](const Record& r) { return Equal(r.reference, path); });
        if (found == g.records.end() && !DirectDesktop(path)) return DesktopManageResult::Reference;
        if (!g.ready || path.empty()) return DesktopManageResult::Failed;
        Record* record = found == g.records.end() ? nullptr : &*found;
        if (record) {
            if (!Reconcile(*record)) return DesktopManageResult::Failed;
            if (record->phase == Phase::Managed) return DesktopManageResult::Managed;
        }
        const auto source = record ? record->actual : path;
        // A portable executable placed on Desktop must remain at the path used
        // by autostart and desktop shell commands, even if a card references it.
        if (Equal(source, Normalize(ExecutablePath()))) return DesktopManageResult::Reference;
        if (!DirectDesktop(source)) return DesktopManageResult::Reference;
        if (!Exists(source) || g.records.size() >= kMaxRecords) return DesktopManageResult::Failed;
        // Delete rights on an existing public entry do not imply the right to
        // add it back. Check both restoration capabilities before taking it.
        if (!RestoreWritable(source)) {
            const DWORD error=GetLastError();
            if(error!=ERROR_ACCESS_DENIED)return DesktopManageResult::Failed;
            // The constrained helper intentionally rejects reparse-point
            // payloads. Never acquire one if we would need it for restoration.
            const DWORD attributes=GetFileAttributesW(source.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return DesktopManageResult::Failed;
            if(g.injected||!FileHelperRunning()||CheckPrivilegedDesktopRestore(Parent(source))!=ERROR_SUCCESS)
                return DesktopManageResult::RequiresPermission;
        }
        const auto storage = StorageFor(source);
        if (storage.empty()) return DesktopManageResult::Failed;
        std::wstring destination;
        for (unsigned i = 0; i < 100000; ++i) {
            const auto itemDirectory = Join(storage, std::to_wstring(Hash(path)) + L"-" + std::to_wstring(i));
            // Reserve a new directory atomically, preserving the original item
            // basename for Shell labels and Explorer drag/drop destinations.
            if (CreateDirectoryW(itemDirectory.c_str(), nullptr)) {
                destination = Join(itemDirectory, Name(source)); break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)
                return DesktopManageResult::Failed;
        }
        if (destination.empty()) return DesktopManageResult::Failed;
        if (!record) { g.records.push_back({path, destination, source, Phase::Moving}); record = &g.records.back(); }
        else { record->actual = destination; record->target = source; record->phase = Phase::Moving; }
        if (!Persist()) return DesktopManageResult::Failed;
        const DWORD moved=RenameDesktopFile(source,destination);
        if (moved!=ERROR_SUCCESS) {
            if(moved==ERROR_IO_INCOMPLETE)return DesktopManageResult::Failed;
            record->actual = source; record->phase = Phase::Alias; Persist();
            return moved==ERROR_ACCESS_DENIED?DesktopManageResult::RequiresPermission:DesktopManageResult::Failed;
        }
        record->phase = Phase::Managed;
        // The pre-move intent remains recoverable if a later journal update fails.
        Persist();
        return DesktopManageResult::Managed;
    } catch (...) { return DesktopManageResult::Failed; }
}
std::wstring ResolveDesktopItem(const std::wstring& reference) {
    Lock lock(false);
    try {
        const auto path = Normalize(reference);
        const auto* found = FindResolvedRecord(path);
        if (!found) return reference;
        return ActualFor(*found) + path.substr(found->reference.size());
    } catch (...) { return reference; }
}
std::wstring ReferenceForDesktopItem(const std::wstring& actualPath) {
    Lock lock(false);
    try {
        const auto path = Normalize(actualPath);
        const Record* best = nullptr;
        size_t bestLength = 0;
        for (const auto& record : g.records) {
            const auto actual = ActualFor(record);
            if (Equal(actual, path)) return record.reference;
            if (Prefix(actual, path) && actual.size() > bestLength) {
                best = &record; bestLength = actual.size();
            }
        }
        return best ? best->reference + path.substr(bestLength) : actualPath;
    } catch (...) { return actualPath; }
}
bool IsManagedDesktopItem(const std::wstring& reference) {
    if(IsHiddenDesktopItem(ResolveDesktopItem(reference)))return true;
    Lock lock(false);
    try {
        const auto path = Normalize(reference);
        const auto found = std::find_if(g.records.begin(), g.records.end(), [&](const Record& r) { return Equal(r.reference, path); });
        if (found == g.records.end() || found->phase == Phase::Alias) return false;
        if (found->phase == Phase::Managed) return true;
        if (found->phase == Phase::Moving && Exists(found->target)) return false;
        return Exists(found->actual);
    } catch (...) { return false; }
}
bool ReleaseDesktopItem(const std::wstring& reference) {
    Lock lock(true);
    try {
        g.permissionRequired=false;
        if(!g.injected&&FileHelperOperationPending())return false;
        const auto path = Normalize(reference);
        const auto found = std::find_if(g.records.begin(), g.records.end(), [&](const Record& r) { return Equal(r.reference, path); });
        const bool legacy=found == g.records.end() || (g.ready && Restore(*found));
        return legacy&&(g.injected||ReleaseHiddenDesktopItem(found==g.records.end()?reference:ActualFor(*found)));
    } catch (...) { return false; }
}
bool RestoreDesktopItems() {
    Lock lock(true);
    try {
        g.permissionRequired=false;
        if(!g.injected&&FileHelperOperationPending())return false;
        // An unreadable or unwritable journal cannot establish that there are
        // no owned entries. Report failure even when parsing yielded no rows.
        if (!g.ready) {if(!g.injected)RestoreHiddenItems();return false;}
        bool success = true;
        for (auto& record : g.records) if (!Restore(record)) success = false;
        return (g.injected||RestoreHiddenItems())&&success;
    } catch (...) { return false; }
}
bool DesktopItemsNeedPermission() {Lock lock(false);return g.permissionRequired||HiddenItemsNeedPermission();}
void ClearDesktopItems() { Lock lock(true); g = {};ClearHiddenItems(); }

namespace testing {
bool InitializeDesktopItemsAt(const std::wstring& directory, const std::wstring& desktop,
    const std::wstring& publicDesktop, const std::wstring& storage) {
    Lock lock(true);
    try { return Initialize(directory, desktop, publicDesktop, storage, true); }
    catch (...) { g.ready = false; return false; }
}
} // namespace testing
} // namespace desk
