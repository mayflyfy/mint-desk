#include "../src/desktop_items.h"
#include "../src/state.h"
#include <aclapi.h>
#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace {
int failures = 0;
constexpr UINT kShellChanged=WM_APP+423;
std::wstring watchedSource;
ULONGLONG shellNoticeAt=0;
unsigned noticeMessages=0;
LONG noticeKinds=0;
LRESULT CALLBACK NoticeWindow(HWND h,UINT message,WPARAM wp,LPARAM lp) {
    if(message==kShellChanged) {
        ++noticeMessages;
        PIDLIST_ABSOLUTE* items=nullptr;LONG event=0;
        HANDLE lock=SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp),static_cast<DWORD>(lp),&items,&event);
        if(lock) {
            noticeKinds|=event;
            wchar_t path[MAX_PATH]{};
            if((event&(SHCNE_RENAMEITEM|SHCNE_RENAMEFOLDER|SHCNE_DELETE|SHCNE_RMDIR))&&items&&items[0]&&SHGetPathFromIDListW(items[0],path)&&!_wcsicmp(path,watchedSource.c_str()))shellNoticeAt=GetTickCount64();
            SHChangeNotification_Unlock(lock);
        }
        return 0;
    }
    return DefWindowProcW(h,message,wp,lp);
}
void Check(bool value, const char* description) {
    if (!value) { std::fprintf(stderr, "FAIL: %s (Win32 %lu)\n", description, GetLastError()); ++failures; }
}
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void MakeDirectory(const std::wstring& path) { Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "create test directory"); }
void Write(const std::wstring& path, const std::string& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create fixture");
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    Check(WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size(), "write fixture");
    CloseHandle(file);
}
std::string Read(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(file, nullptr), read = 0;
    std::string data(size, '\0');
    if (!ReadFile(file, data.data(), size, &read, nullptr) || read != size) data.clear();
    CloseHandle(file); return data;
}
uint32_t Crc(const unsigned char* bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
void Put32(std::string& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
}
// Change the first record's durable phase to exercise crash reconciliation.
void PhaseFixture(const std::wstring& journal, uint32_t phase) {
    auto bytes = Read(journal);
    Check(bytes.size() > 28, "phase fixture exists");
    if (bytes.size() <= 28) return;
    Put32(bytes, 24, phase);
    Put32(bytes, 16, Crc(reinterpret_cast<const unsigned char*>(bytes.data() + 20), bytes.size() - 20));
    Write(journal, bytes);
}
struct DenyDirectoryAdds {
    std::wstring path;
    PSECURITY_DESCRIPTOR original = nullptr;
    PACL originalDacl = nullptr;
    bool applied = false;
    explicit DenyDirectoryAdds(const std::wstring& directory) : path(directory) {
        if (GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &originalDacl, nullptr, &original) != ERROR_SUCCESS) return;
        DWORD sidBuffer[(SECURITY_MAX_SID_SIZE + sizeof(DWORD) - 1) / sizeof(DWORD)]{};
        DWORD sidSize = sizeof(sidBuffer);
        if (!CreateWellKnownSid(WinWorldSid, nullptr, sidBuffer, &sidSize)) return;
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = reinterpret_cast<wchar_t*>(sidBuffer);
        PACL updated = nullptr;
        if (SetEntriesInAclW(1, &deny, originalDacl, &updated) != ERROR_SUCCESS) return;
        applied = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr) == ERROR_SUCCESS;
        LocalFree(updated);
    }
    ~DenyDirectoryAdds() {
        if (applied) Check(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, originalDacl, nullptr) == ERROR_SUCCESS, "restore isolated ACL fixture");
        if (original) LocalFree(original);
    }
};
void Cleanup(const std::wstring& directory) {
    // Strictly constrained to the explicit isolated test root and its children.
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            const auto child = directory + L"\\" + data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Cleanup(child);
            else { SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(child.c_str()); }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(directory.c_str());
}
void RecoveryLifecycle(const std::wstring& root, const std::wstring& desktop, const std::wstring& common) {
    using namespace desk;
    const auto config = root + L"\\LifecycleConfig", storage = root + L"\\LifecycleManaged";
    MakeDirectory(config); MakeDirectory(storage);
    auto initialize = [&] { return testing::InitializeDesktopItemsAt(config, desktop, common, storage); };
    Check(initialize(), "initialize lifecycle recovery fixture");
    const auto healthy = desktop + L"\\lifecycle-healthy.txt", busy = desktop + L"\\lifecycle-busy.txt",
        unsaved = desktop + L"\\lifecycle-unsaved.txt";
    Write(healthy, "healthy exact data"); Write(busy, "busy exact data"); Write(unsaved, "unsaved import data");
    AppState state; state.nextId = 2;
    CardState card; card.id = 1; card.title = L"Saved layout"; card.paths = {healthy, busy};
    state.cards.push_back(card);
    Check(testing::SaveStateToDirectory(config, state), "persist original references before simulated startup");
    Check(ManageDesktopItem(healthy) == DesktopManageResult::Managed && ManageDesktopItem(busy) == DesktopManageResult::Managed &&
        ManageDesktopItem(unsaved) == DesktopManageResult::Managed, "acquire saved entries and an import not yet saved in cards");
    const auto busyManaged = ResolveDesktopItem(busy);
    HANDLE held = CreateFileW(busyManaged.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold managed item without delete sharing");
    Check(!RestoreDesktopItems(), "exit reports partial restore failure for a busy managed file");
    Check(Read(healthy) == "healthy exact data" && Read(unsaved) == "unsaved import data", "failed exit still restores healthy and unsaved orphan entries");
    Check(!Exists(busy) && Read(busyManaged) == "busy exact data" && ResolveDesktopItem(busy) == busyManaged,
        "failed exit retains busy file and its recoverable mapping without deleting content");
    ClearDesktopItems();
    Check(initialize() && !RestoreDesktopItems(), "restart reconciles durable restoring intent and retries held file safely");
    Check(Read(healthy) == "healthy exact data" && Read(busyManaged) == "busy exact data", "repeated recovery failure preserves restored and managed bytes");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(RestoreDesktopItems() && Read(busy) == "busy exact data", "closing occupying handle allows complete restore retry");
    AppState loaded;
    Check(testing::LoadStateFromDirectory(config, loaded) && loaded.cards.size() == 1 && loaded.cards[0].paths == card.paths,
        "recovery leaves the last saved card references and layout intact");
    for (const auto& reference : loaded.cards[0].paths)
        Check(ManageDesktopItem(reference) == DesktopManageResult::Managed, "complete recovery may reapply each last-saved card reference");
    Check(!Exists(healthy) && !Exists(busy) && Read(unsaved) == "unsaved import data", "reapply hides saved entries while recovered unsaved orphan stays visible");
    Check(RestoreDesktopItems() && Read(healthy) == "healthy exact data" && Read(busy) == "busy exact data", "subsequent normal exit restores the saved layout's files");
    ClearDesktopItems();

    // Restore permissions can change after the acquisition preflight succeeded.
    Check(initialize() && ManageDesktopItem(healthy) == DesktopManageResult::Managed, "prepare changed-permission recovery fixture");
    const auto permissionManaged = ResolveDesktopItem(healthy);
    {
        DenyDirectoryAdds deny(desktop);
        Check(deny.applied, "deny adds only on isolated desktop during exit");
        if (deny.applied) {
            Check(!RestoreDesktopItems() && Read(permissionManaged) == "healthy exact data" && !Exists(healthy),
                "permissions revoked after acquisition leave sole managed copy intact");
            ClearDesktopItems();
            Check(initialize() && !RestoreDesktopItems() && Read(permissionManaged) == "healthy exact data",
                "restart preserves and retries file while desktop restore permission remains denied");
        }
    }
    Check(RestoreDesktopItems() && Read(healthy) == "healthy exact data", "restoring directory permission recovers file on retry");
    ClearDesktopItems();
}

void RestoreCrashCollision(const std::wstring& root, const std::wstring& desktop, const std::wstring& common) {
    using namespace desk;
    const auto config = root + L"\\RestoreCrashConfig", storage = root + L"\\RestoreCrashManaged";
    MakeDirectory(config); MakeDirectory(storage);
    auto initialize = [&] { return testing::InitializeDesktopItemsAt(config, desktop, common, storage); };
    Check(initialize(), "initialize restore collision crash fixture");
    const auto reference = desktop + L"\\restore-crash.txt";
    Write(reference, "owned restore bytes");
    Check(ManageDesktopItem(reference) == DesktopManageResult::Managed, "prepare restoring-phase collision");
    const auto managed = ResolveDesktopItem(reference);
    Write(reference, "externally created target bytes");
    PhaseFixture(config + L"\\desktop-items.dat", 3);
    ClearDesktopItems();
    Check(initialize() && RestoreDesktopItems(), "pending restore with both names present retries into unique collision alias");
    const auto alias = ResolveDesktopItem(reference);
    Check(alias != reference && Read(alias) == "owned restore bytes" && Read(reference) == "externally created target bytes" && !Exists(managed),
        "restore crash never overwrites external target or loses the managed copy");
    ClearDesktopItems();
    Check(initialize() && Read(ResolveDesktopItem(reference)) == "owned restore bytes", "restoring-phase collision alias persists through reload");
    ClearDesktopItems();
}
void DoubleJournalRecovery(const std::wstring& root, const std::wstring& desktop, const std::wstring& common) {
    using namespace desk;
    const auto config = root + L"\\DoubleJournalConfig", storage = root + L"\\DoubleJournalManaged";
    MakeDirectory(config); MakeDirectory(storage);
    auto initialize = [&] { return testing::InitializeDesktopItemsAt(config, desktop, common, storage); };
    Check(initialize(), "initialize double journal recovery fixture");
    const auto reference = desktop + L"\\double-journal.txt", untouched = desktop + L"\\double-journal-untouched.txt";
    Write(reference, "sole managed copy survives metadata damage"); Write(untouched, "still on desktop");
    Check(ManageDesktopItem(reference) == DesktopManageResult::Managed, "prepare sole managed copy before metadata corruption");
    const auto actual = ResolveDesktopItem(reference), primary = config + L"\\desktop-items.dat", backup = config + L"\\desktop-items.backup.dat";
    const auto primaryBytes = Read(primary), backupBytes = Read(backup);
    Write(primary, "corrupt primary"); Write(backup, "corrupt backup");
    ClearDesktopItems();
    Check(!initialize() && !RestoreDesktopItems(), "both corrupt journals report failed initialization and failed restoration");
    Check(Read(actual) == "sole managed copy survives metadata damage" && !Exists(reference), "metadata damage never deletes or overwrites the sole stored copy");
    Check(ManageDesktopItem(untouched) == DesktopManageResult::Failed && Read(untouched) == "still on desktop", "metadata recovery failure cannot acquire more desktop files");
    // Recovery here is explicit: restore previously saved journal bytes. No
    // automatic guessing, renaming or deleting of unknown storage is performed.
    Write(primary, primaryBytes); Write(backup, backupBytes);
    ClearDesktopItems();
    Check(initialize() && RestoreDesktopItems() && Read(reference) == "sole managed copy survives metadata damage", "restoring intact metadata permits safe recovery of retained managed bytes");
    ClearDesktopItems();
}
void MissingOrdinaryAliases(const std::wstring& root, const std::wstring& desktop, const std::wstring& common) {
    using namespace desk;
    const auto config = root + L"\\MissingAliasConfig", storage = root + L"\\MissingAliasManaged";
    MakeDirectory(config); MakeDirectory(storage);
    auto initialize = [&] { return testing::InitializeDesktopItemsAt(config, desktop, common, storage); };
    Check(initialize(), "initialize missing ordinary aliases fixture");
    const auto moved = desktop + L"\\alias-user-moved.txt", removed = desktop + L"\\alias-user-deleted.txt",
        held = desktop + L"\\alias-real-managed.txt", outside = root + L"\\user-moved-ordinary-file.txt";
    Write(moved, "user moved ordinary file"); Write(removed, "user deleted ordinary file");
    Check(ManageDesktopItem(moved) == DesktopManageResult::Managed && ManageDesktopItem(removed) == DesktopManageResult::Managed && RestoreDesktopItems(),
        "return both aliases to ordinary desktop ownership");
    Check(MoveFileExW(moved.c_str(), outside.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "simulate user moving an already restored ordinary file");
    Check(DeleteFileW(removed.c_str()) != FALSE, "simulate user deleting only an already restored isolated fixture");
    ClearDesktopItems();
    Check(initialize() && RestoreDesktopItems() && ReleaseDesktopItem(moved) && ReleaseDesktopItem(removed),
        "missing non-managed aliases do not block initialization, release, or normal exit restoration");
    Check(Read(outside) == "user moved ordinary file" && !Exists(moved) && !Exists(removed), "ordinary alias handling neither recreates deleted files nor moves user-relocated data");
    Write(held, "real managed data");
    Check(ManageDesktopItem(held) == DesktopManageResult::Managed, "prepare genuinely owned missing file");
    const auto ownedPath = ResolveDesktopItem(held), parked = root + L"\\parked-real-managed-file.txt";
    Check(MoveFileExW(ownedPath.c_str(), parked.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "temporarily park genuinely managed file");
    ClearDesktopItems();
    Check(!initialize() && !RestoreDesktopItems(), "genuinely missing managed data still blocks recovery success");
    Check(Read(parked) == "real managed data", "managed missing-file failure never deletes parked bytes");
    Check(MoveFileExW(parked.c_str(), ownedPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "return missing managed fixture to its owned path");
    ClearDesktopItems();
    Check(initialize() && RestoreDesktopItems() && Read(held) == "real managed data", "returning owned data restores recovery while stale aliases remain harmless");
    ClearDesktopItems();
}
void ImmediateShellNotice(const std::wstring& root,const std::wstring& desktop,const std::wstring& common) {
    using namespace desk;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const auto config=root+L"\\NoticeConfig",storage=root+L"\\NoticeManaged";
    wchar_t expanded[32768]{};
    const std::wstring desktopPath=GetLongPathNameW(desktop.c_str(),expanded,32768)?expanded:desktop;
    MakeDirectory(config);MakeDirectory(storage);
    Check(testing::InitializeDesktopItemsAt(config,desktopPath,common,storage),"initialize isolated shell-notification fixture");
    WNDCLASSW c{};c.lpfnWndProc=NoticeWindow;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"MintDesk.NoticeTest";
    RegisterClassW(&c);
    HWND window=CreateWindowW(c.lpszClassName,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,c.hInstance,nullptr);
    PIDLIST_ABSOLUTE directory=ILCreateFromPathW(desktopPath.c_str());
    SHChangeNotifyEntry entry{directory,TRUE};
    const ULONG registration=SHChangeNotifyRegister(window,SHCNRF_ShellLevel|SHCNRF_InterruptLevel|SHCNRF_NewDelivery,SHCNE_ALLEVENTS,kShellChanged,1,&entry);
    Check(window&&directory&&registration,"subscribe to exact shell rename notifications");
    ULONGLONG total=0,maximum=0;
    for(int i=0;i<10&&registration;++i) {
        watchedSource=desktopPath+L"\\notify-"+std::to_wstring(i)+L".txt";Write(watchedSource,"notification payload");shellNoticeAt=0;
        const ULONGLONG start=GetTickCount64();
        Check(ManageDesktopItem(watchedSource)==DesktopManageResult::Managed,"notification sample moves only its isolated payload");
        while(!shellNoticeAt&&GetTickCount64()-start<2000) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            if(!shellNoticeAt)Sleep(1);
        }
        Check(shellNoticeAt>=start,"moving a desktop item immediately publishes its precise removal to Shell subscribers");
        if(shellNoticeAt>=start){const auto elapsed=shellNoticeAt-start;total+=elapsed;maximum=std::max(maximum,elapsed);}
    }
    std::printf("Shell item notice: 10 imports, mean %.1f ms, max %llu ms (durable journal included; messages %u, events %lx).\n",total/10.0,static_cast<unsigned long long>(maximum),noticeMessages,static_cast<unsigned long>(noticeKinds));
    if(registration)SHChangeNotifyDeregister(registration);
    if(directory)CoTaskMemFree(directory);
    if(window)DestroyWindow(window);
    Check(RestoreDesktopItems(),"restore every notification fixture before cleanup");ClearDesktopItems();
    if(SUCCEEDED(com))CoUninitialize();
}
}

int main() {
    using namespace desk;
    wchar_t temp[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "temp path");
    const std::wstring root = std::wstring(temp) + L"MintDesk-items-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto config = root + L"\\Config", desktop = root + L"\\Desktop", common = root + L"\\Public", storage = root + L"\\Managed";
    MakeDirectory(root); MakeDirectory(config); MakeDirectory(desktop); MakeDirectory(common); MakeDirectory(storage);
    const auto journal = config + L"\\desktop-items.dat", backup = config + L"\\desktop-items.backup.dat";
    auto initialize = [&] { return testing::InitializeDesktopItemsAt(config, desktop, common, storage); };
    Check(initialize(), "initialize explicit isolated roots");
    const auto original = desktop + L"\\工具.lnk";
    Write(original, "shortcut payload");
    Check(ManageDesktopItem(original) == DesktopManageResult::Managed, "desktop item acquired");
    const auto managed = ResolveDesktopItem(original);
    Check(!Exists(original) && Exists(managed) && managed.find(storage) == 0, "desktop entry disappears into storage");
    Check(managed.substr(managed.find_last_of(L'\\') + 1) == original.substr(original.find_last_of(L'\\') + 1), "managed item preserves original basename");
    Check(IsManagedDesktopItem(original), "exact reference reports ownership");
    Check(ReferenceForDesktopItem(managed) == original, "native dragged managed file maps back to persistent reference");
    Check(Read(managed) == "shortcut payload", "moving preserves exact file content");
    Check(ManageDesktopItem(original) == DesktopManageResult::Managed && ResolveDesktopItem(original) == managed, "duplicate manage is idempotent");
    const auto external = root + L"\\external.txt";
    Write(external, "external");
    Check(ManageDesktopItem(external) == DesktopManageResult::Reference && ResolveDesktopItem(external) == external && Exists(external), "external file remains a reference");
    Check(ReferenceForDesktopItem(external) == external, "reverse resolution preserves ordinary external paths");
    const auto folder = desktop + L"\\Folder", nested = folder + L"\\nested.txt";
    MakeDirectory(folder); Write(nested, "nested");
    Check(ManageDesktopItem(nested) == DesktopManageResult::Reference && Exists(nested), "nested desktop child is not managed independently");
    ClearDesktopItems();
    Check(initialize() && ResolveDesktopItem(original) == managed && Exists(managed), "journal reload recovers managed file");
    // Intent-before-rename crash with the destination already present.
    PhaseFixture(journal, 1);
    ClearDesktopItems();
    Check(initialize() && ResolveDesktopItem(original) == managed, "pending move after successful rename reconciles");
    Write(original, "new unrelated desktop file");
    Check(ReleaseDesktopItem(original), "release restores despite filename collision");
    const auto alias = ResolveDesktopItem(original);
    Check(alias != original && alias.find(L" (MintDesk 1)") != std::wstring::npos && Exists(alias), "collision receives unique restored name");
    Check(!IsManagedDesktopItem(original), "restored alias reports no managed ownership");
    Check(Read(original) == "new unrelated desktop file" && Read(alias) == "shortcut payload", "restore never overwrites either file");
    ClearDesktopItems();
    Check(initialize() && ResolveDesktopItem(original) == alias, "old card reference resolves persistent restoration alias");
    Check(ReferenceForDesktopItem(alias) == original, "reloaded collision alias reverses to original card reference");
    Check(ManageDesktopItem(original) == DesktopManageResult::Managed && !Exists(alias) && Read(original) == "new unrelated desktop file", "reopening manages alias while preserving replacement desktop file");
    Check(ReleaseDesktopItem(original) && ResolveDesktopItem(original) == alias && Read(alias) == "shortcut payload", "alias restored to its own prior name");

    const auto publicItem = common + L"\\Public.txt";
    Write(publicItem, "public");
    Check(ManageDesktopItem(publicItem) == DesktopManageResult::Managed && !Exists(publicItem), "public desktop direct child managed");
    Check(RestoreDesktopItems() && Exists(publicItem), "normal exit restores all desktop entries");
    // Readonly journal failure must occur before the source can move.
    const auto protectedItem = desktop + L"\\protected.txt";
    Write(protectedItem, "protected");
    Check(SetFileAttributesW(journal.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "protect journal");
    Check(ManageDesktopItem(protectedItem) == DesktopManageResult::Failed && Exists(protectedItem) && Read(protectedItem) == "protected", "failed durable intent never moves a source");
    SetFileAttributesW(journal.c_str(), FILE_ATTRIBUTE_NORMAL);
    ClearDesktopItems(); Check(initialize(), "reload after rejected intent");

    const auto locked = desktop + L"\\locked.txt";
    Write(locked, "locked");
    HANDLE held = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold exclusive source");
    Check(ManageDesktopItem(locked) == DesktopManageResult::Failed && Exists(locked), "rename sharing violation preserves source");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(ResolveDesktopItem(locked) == locked && Read(locked) == "locked", "failed rename resolves unchanged source");
    Check(ManageDesktopItem(locked) == DesktopManageResult::Managed, "retry after sharing violation succeeds");
    const auto lockedManaged = ResolveDesktopItem(locked);
    // Invalid primary recovers the already-durable identical backup.
    Write(journal, "corrupt"); ClearDesktopItems();
    Check(initialize() && ResolveDesktopItem(locked) == lockedManaged && Exists(lockedManaged), "corrupted journal recovers durable backup");
    Check(RestoreDesktopItems() && Read(locked) == "locked", "backup recovery restores original data");

    // A planned move that never happened must retain the source.
    const auto pendingConfig = root + L"\\PendingConfig", pendingStorage = root + L"\\PendingManaged";
    MakeDirectory(pendingConfig); MakeDirectory(pendingStorage);
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage), "fresh pending fixture");
    const auto pending = desktop + L"\\pending.txt"; Write(pending, "pending");
    Check(ManageDesktopItem(pending) == DesktopManageResult::Managed, "prepare pending fixture");
    const auto pendingManaged = ResolveDesktopItem(pending);
    Check(MoveFileExW(pendingManaged.c_str(), pending.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "simulate rename not committed");
    PhaseFixture(pendingConfig + L"\\desktop-items.dat", 1);
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage) && ResolveDesktopItem(pending) == pending && Read(pending) == "pending", "pending intent with source only reconciles safely");
    Check(ManageDesktopItem(pending) == DesktopManageResult::Managed, "prepare restoring intent");
    const auto restoringManaged = ResolveDesktopItem(pending);
    Check(MoveFileExW(restoringManaged.c_str(), pending.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "simulate restoration committed before phase update");
    PhaseFixture(pendingConfig + L"\\desktop-items.dat", 3);
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage) && ResolveDesktopItem(pending) == pending && Read(pending) == "pending", "committed restore intent recovers original file");
    Check(ManageDesktopItem(pending) == DesktopManageResult::Managed, "prepare temporarily missing managed item");
    const auto missingManaged = ResolveDesktopItem(pending), parked = root + L"\\parked.txt";
    Check(MoveFileExW(missingManaged.c_str(), parked.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "park managed fixture");
    ClearDesktopItems();
    Check(!testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage), "missing source and destination reports failure");
    Check(ResolveDesktopItem(pending) == missingManaged && Read(parked) == "pending", "missing record kept without deleting unrelated data");
    Check(MoveFileExW(parked.c_str(), missingManaged.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "return temporarily missing fixture");
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage) && RestoreDesktopItems() && Read(pending) == "pending", "missing record recovers on next initialization");

    const auto ownedFolder = desktop + L"\\OwnedFolder", ownedChild = ownedFolder + L"\\child.txt";
    MakeDirectory(ownedFolder); Write(ownedChild, "directory content");
    Check(ManageDesktopItem(ownedFolder) == DesktopManageResult::Managed && !Exists(ownedFolder) &&
        Read(ResolveDesktopItem(ownedFolder) + L"\\child.txt") == "directory content", "whole folder moves atomically with contents intact");
    Check(Read(ResolveDesktopItem(ownedChild)) == "directory content" &&
        ResolveDesktopItem(ownedChild) == ResolveDesktopItem(ownedFolder) + L"\\child.txt", "nested card reference resolves inside managed folder");
    Check(ReferenceForDesktopItem(ResolveDesktopItem(ownedFolder)) == ownedFolder &&
        ReferenceForDesktopItem(ResolveDesktopItem(ownedChild)) == ownedChild, "managed folder and nested native drags round trip ownership identity");
    Check(!IsManagedDesktopItem(ownedChild), "nested reference does not own parent folder");
    Check(ResolveDesktopItem(ownedFolder + L"Else\\child.txt") == ownedFolder + L"Else\\child.txt", "prefix resolution requires a path component boundary");
    Check(ReleaseDesktopItem(ownedFolder) && Read(ownedChild) == "directory content", "folder restoration keeps all contents");
    Check(ResolveDesktopItem(ownedChild) == ownedChild, "nested reference follows restored parent");
    Check(ManageDesktopItem(ownedFolder) == DesktopManageResult::Managed, "manage folder again for collision");
    MakeDirectory(ownedFolder); Write(ownedChild, "unrelated replacement directory");
    Check(RestoreDesktopItems(), "restore all with directory collision");
    const auto folderAlias = ResolveDesktopItem(ownedFolder);
    Check(folderAlias != ownedFolder && Read(ResolveDesktopItem(ownedChild)) == "directory content" &&
        Read(ownedChild) == "unrelated replacement directory", "nested reference follows collision alias without overwriting replacement directory");
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage) &&
        ResolveDesktopItem(ownedChild) == folderAlias + L"\\child.txt", "nested collision alias survives journal reload");
    Check(ReferenceForDesktopItem(folderAlias) == ownedFolder &&
        ReferenceForDesktopItem(folderAlias + L"\\child.txt") == ownedChild, "reloaded folder collision aliases reverse original and nested identities");

    // If neither durable journal can be read, do not acquire any further file.
    Write(pendingConfig + L"\\desktop-items.dat", "corrupt");
    Write(pendingConfig + L"\\desktop-items.backup.dat", "also corrupt");
    ClearDesktopItems();
    Check(!testing::InitializeDesktopItemsAt(pendingConfig, desktop, common, pendingStorage), "both corrupt journals fail closed");
    Check(!RestoreDesktopItems(), "unrecoverable metadata cannot falsely report successful restoration");
    Check(ManageDesktopItem(pending) == DesktopManageResult::Failed && Read(pending) == "pending", "unrecoverable journal cannot move desktop files");
    ClearDesktopItems();

    const auto ambiguousConfig = root + L"\\AmbiguousConfig", ambiguousStorage = root + L"\\AmbiguousStorage";
    MakeDirectory(ambiguousConfig); MakeDirectory(ambiguousStorage);
    Check(testing::InitializeDesktopItemsAt(ambiguousConfig, desktop, common, ambiguousStorage), "initialize ambiguous move fixture");
    const auto ambiguous = desktop + L"\\ambiguous.txt";
    Write(ambiguous, "original desktop data");
    Check(ManageDesktopItem(ambiguous) == DesktopManageResult::Managed, "prepare ambiguous move");
    const auto ambiguousDestination = ResolveDesktopItem(ambiguous);
    Check(MoveFileExW(ambiguousDestination.c_str(), ambiguous.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "return source to pre-move name");
    Write(ambiguousDestination, "unrelated destination data");
    PhaseFixture(ambiguousConfig + L"\\desktop-items.dat", 1);
    ClearDesktopItems();
    Check(!testing::InitializeDesktopItemsAt(ambiguousConfig, desktop, common, ambiguousStorage), "ambiguous pending move fails initialization safely");
    Check(ResolveDesktopItem(ambiguous) == ambiguous && !IsManagedDesktopItem(ambiguous), "ambiguous move resolves original source without claiming ownership");
    Check(ManageDesktopItem(ambiguous) == DesktopManageResult::Failed && !ReleaseDesktopItem(ambiguous) && !RestoreDesktopItems(), "ambiguous move refuses acquisition or restoration");
    Check(Read(ambiguous) == "original desktop data" && Read(ambiguousDestination) == "unrelated destination data", "ambiguous move preserves both files byte for byte");
    ClearDesktopItems();

    const auto aclConfig = root + L"\\AclConfig", aclStorage = root + L"\\AclStorage", aclPublic = root + L"\\AclPublicDesktop";
    MakeDirectory(aclConfig); MakeDirectory(aclStorage); MakeDirectory(aclPublic);
    Check(testing::InitializeDesktopItemsAt(aclConfig, desktop, aclPublic, aclStorage), "initialize isolated public permissions fixture");
    const auto publicOrphan = aclPublic + L"\\RestoredCommon.txt", deniedFile = aclPublic + L"\\Denied.txt",
        deniedFolder = aclPublic + L"\\DeniedFolder", publicCollision = desktop + L"\\RestoredCommon.txt";
    Write(publicOrphan, "recover public file"); Write(deniedFile, "must stay public");
    MakeDirectory(deniedFolder); Write(deniedFolder + L"\\child.txt", "must stay inside public folder");
    Write(publicCollision, "unrelated user desktop file");
    Check(ManageDesktopItem(publicOrphan) == DesktopManageResult::Managed, "prepare existing owned public entry before restricting adds");
    {
        DenyDirectoryAdds deny(aclPublic);
        Check(deny.applied, "apply isolated public add-rights denial");
        if (deny.applied) {
            Check(ManageDesktopItem(deniedFile) == DesktopManageResult::RequiresPermission && Read(deniedFile) == "must stay public", "restore-rights preflight requests permission while preserving the public file");
            Check(ManageDesktopItem(deniedFolder) == DesktopManageResult::RequiresPermission &&
                Read(deniedFolder + L"\\child.txt") == "must stay inside public folder", "restore-rights preflight leaves non-restorable public folder intact");
            Check(!RestoreDesktopItems(), "public restore waits for permission instead of silently changing its desktop location");
            const auto recovered = ResolveDesktopItem(publicOrphan);
            Check(recovered.find(aclStorage + L"\\") == 0 && Read(recovered) == "recover public file" &&
                Read(publicCollision) == "unrelated user desktop file", "a failed protected restore keeps the only copy and leaves other desktop entries intact");
            Check(ReferenceForDesktopItem(recovered) == publicOrphan && IsManagedDesktopItem(publicOrphan), "pending public restore keeps original reference identity");
        }
    }
    Write(publicOrphan,"unrelated public replacement");
    Check(RestoreDesktopItems(),"public restore succeeds after permission is returned");
    const auto repairedSource = ResolveDesktopItem(publicOrphan);
    Check(repairedSource!=publicOrphan&&Read(publicOrphan)=="unrelated public replacement"&&Read(repairedSource)=="recover public file",
        "public restoration never overwrites a replacement desktop entry");
    Check(DeleteFileW(publicOrphan.c_str())!=FALSE,"remove only the explicit synthetic replacement fixture");
    Check(MoveFileExW(repairedSource.c_str(), publicOrphan.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
        "simulate explicit external administrator repair to original public path");
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(aclConfig, desktop, aclPublic, aclStorage) &&
        ResolveDesktopItem(publicOrphan) == publicOrphan && Read(publicOrphan) == "recover public file",
        "alias metadata follows externally repaired original public desktop path");
    ClearDesktopItems();
    Check(testing::InitializeDesktopItemsAt(aclConfig, desktop, aclPublic, aclStorage) &&
        ResolveDesktopItem(publicOrphan) == publicOrphan && ReferenceForDesktopItem(publicOrphan) == publicOrphan,
        "external repair metadata persists across a second journal reload");
    ClearDesktopItems();
    RecoveryLifecycle(root, desktop, common);
    RestoreCrashCollision(root, desktop, common);
    DoubleJournalRecovery(root, desktop, common);
    MissingOrdinaryAliases(root, desktop, common);
    ImmediateShellNotice(root,desktop,common);
    Check(Exists(backup), "backup journal retained");
    Cleanup(root);
    if (!failures) std::puts("All desktop ownership tests passed.");
    return failures ? 1 : 0;
}
