#include "../src/hidden_items.h"
#include "../src/visibility.h"
#include "../src/file_helper.h"
#include "../src/state.h"
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures=0;
void Check(bool value,const char* name){if(!value){std::fprintf(stderr,"FAIL: %s (error %lu)\n",name,GetLastError());++failures;}}
void Write(const std::wstring& path){HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);Check(file!=INVALID_HANDLE_VALUE,"create isolated file");if(file==INVALID_HANDLE_VALUE)return;const char bytes[]="never move or change these bytes";DWORD count=0;Check(WriteFile(file,bytes,sizeof(bytes),&count,nullptr)&&count==sizeof(bytes),"write payload");CloseHandle(file);}
bool Content(const std::wstring& path){HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return false;char bytes[64]{};DWORD count=0;const bool ok=ReadFile(file,bytes,sizeof(bytes),&count,nullptr)&&!std::strcmp(bytes,"never move or change these bytes");CloseHandle(file);return ok;}
DWORD Bits(const std::wstring& path){return GetFileAttributesW(path.c_str())&desk::kVisibilityBits;}
void Corrupt(const std::wstring& path){HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);Check(file!=INVALID_HANDLE_VALUE,"open only fixture journal for corruption");if(file==INVALID_HANDLE_VALUE)return;const char bytes[]="invalid journal";DWORD count=0;WriteFile(file,bytes,sizeof(bytes),&count,nullptr);CloseHandle(file);}
}
int main() {
    using namespace desk;
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv&&argc==6&&!wcscmp(argv[1],L"/visibility-watch")) {
        const int result=testing::RunVisibilityWatcherAt(wcstoul(argv[2],nullptr,10),argv[3],argv[4],argv[5]);LocalFree(argv);return result;
    }
    if(argv&&argc==6&&!wcscmp(argv[1],L"/abrupt-fixture")) {
        const bool ok=testing::InitializeHiddenItemsAt(argv[2],argv[3],argv[4])&&HideDesktopItem(argv[5])==DesktopManageResult::Managed&&testing::StartHiddenItemsWatcherAt();
        // This isolated child exits without C++ unwinding or restoration.
        // Only the separate watcher can recover the synthetic fixture file.
        ExitProcess(ok?77:78);
    }
    if(argv)LocalFree(argv);
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const auto root=std::wstring(temp)+L"MintDesk-visibility-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto data=root+L"\\Config",desktop=root+L"\\Desktop",common=root+L"\\Public";
    for(const auto& dir:{root,data,desktop,common})Check(CreateDirectoryW(dir.c_str(),nullptr)!=FALSE,"create isolated directory");
    const auto path=desktop+L"\\file.txt",folder=desktop+L"\\busy-folder",child=folder+L"\\data.txt",outside=root+L"\\outside.txt";
    Write(path);Write(outside);Check(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"create busy-folder fixture");Write(child);
    const DWORD original=GetFileAttributesW(path.c_str()),folderOriginal=GetFileAttributesW(folder.c_str()),childOriginal=GetFileAttributesW(child.c_str());
    HANDLE inUse=CreateFileW(folder.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    Check(inUse!=INVALID_HANDLE_VALUE,"hold folder open without delete-sharing, reproducing failed old rename");
    Check(testing::InitializeHiddenItemsAt(data,desktop,common),"initialize dual visibility journal");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed&&Bits(path)==kVisibilityBits&&Content(path),"hide in place with unchanged path and bytes");
    Check(HideDesktopItem(folder)==DesktopManageResult::Managed&&Bits(folder)==kVisibilityBits&&Content(child),"in-use folder hides without requiring rename or delete permissions");
    Check(GetFileAttributesW(child.c_str())==childOriginal,"never recurse into folder contents");
    Check(HideDesktopItem(outside)==DesktopManageResult::Reference&&Bits(outside)==0,"non-desktop paths stay ordinary references");
    Check(HideDesktopItem(child)==DesktopManageResult::Reference&&GetFileAttributesW(child.c_str())==childOriginal,"nested desktop entries are not independently hidden");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed&&ReleaseHiddenDesktopItem(path)&&GetFileAttributesW(path.c_str())==original,"repeat import preserves original bits and release restores them");
    Check(RestoreHiddenItems()&&GetFileAttributesW(folder.c_str())==folderOriginal,"restore only folder root attributes");
    CloseHandle(inUse);
    Check(SetFileAttributesW(path.c_str(),original|FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_READONLY)!=FALSE,"set original hidden/read-only attributes");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"previously hidden read-only files are supported");
    Check(SetFileAttributesW(path.c_str(),GetFileAttributesW(path.c_str())|FILE_ATTRIBUTE_ARCHIVE)!=FALSE,"simulate another program changing an unrelated bit");
    Check(ReleaseHiddenDesktopItem(path)&&Bits(path)==FILE_ATTRIBUTE_HIDDEN&&(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_READONLY)&&(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_ARCHIVE),"restore original visibility while preserving read-only/archive changes");
    SetFileAttributesW(path.c_str(),original);
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"hide for rename test");
    const auto renamed=desktop+L"\\renamed.txt";
    Check(MoveFileExW(path.c_str(),renamed.c_str(),0)!=FALSE,"user may rename file while hidden");
    Write(path);
    Check(ReleaseHiddenDesktopItem(path)&&Bits(renamed)==(original&kVisibilityBits)&&Bits(path)==0&&Content(path)&&Content(renamed),"held identity restores renamed original without changing replacement");
    DeleteFileW(renamed.c_str());
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"hide for restart recovery");
    ClearHiddenItems();
    Check(testing::InitializeHiddenItemsAt(data,desktop,common)&&RestoreHiddenItems()&&Bits(path)==0&&Content(path),"recover original visibility from durable journal after losing process handles");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"hide for stale watcher test");ClearHiddenItems();
    Check(testing::RecoverHiddenItemsAt(data,desktop,common,GetCurrentProcessId()+1)&&Bits(path)==kVisibilityBits,"old watcher must not restore a newer owner's entries");
    Check(testing::RecoverHiddenItemsAt(data,desktop,common,GetCurrentProcessId())&&Bits(path)==0,"matching watcher restores hidden items");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"hide before simulated external replacement");ClearHiddenItems();
    Check(MoveFileExW(path.c_str(),renamed.c_str(),0)!=FALSE,"simulate user moving original while app stopped");Write(path);
    Check(testing::InitializeHiddenItemsAt(data,desktop,common)&&RestoreHiddenItems()&&Bits(path)==0&&Content(path),"restart never modifies a different file created at the same original path");
    SetFileAttributesW(renamed.c_str(),original);DeleteFileW(renamed.c_str());
    // Attribute helper is constrained to exact desktop roots and two bits.
    HANDLE probe=OpenVisibility(path,false);FileIdentity id;DWORD attrs=0;Check(probe!=INVALID_HANDLE_VALUE&&VisibilityInfo(probe,id,attrs),"capture helper file identity");if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);
    Check(testing::DesktopVisibilityAt(desktop,common,path,id,kVisibilityBits)==ERROR_SUCCESS&&Bits(path)==kVisibilityBits,"restricted attribute helper hides exact desktop file");
    FileIdentity wrong=id;++wrong.low;
    Check(testing::DesktopVisibilityAt(desktop,common,path,wrong,0)==ERROR_FILE_INVALID&&Bits(path)==kVisibilityBits,"helper rejects replacement identity");
    Check(testing::DesktopVisibilityAt(desktop,common,path,id,FILE_ATTRIBUTE_READONLY)==ERROR_ACCESS_DENIED,"helper cannot alter unrelated attribute bits");
    Check(testing::DesktopVisibilityAt(desktop,common,outside,id,0)==ERROR_ACCESS_DENIED,"helper cannot modify arbitrary paths");
    Check(testing::DesktopVisibilityAt(desktop,common,path,id,0)==ERROR_SUCCESS&&Bits(path)==0,"helper restores only visibility bits");
    // Fail persistence before changing a new item; the next attempt must not
    // bypass durable intent just because an in-memory record already exists.
    const auto blocker=data+L"\\hidden-items.dat.tmp";
    Check(CreateDirectoryW(blocker.c_str(),nullptr)!=FALSE,"block atomic journal write with fixture directory");
    Check(HideDesktopItem(path)==DesktopManageResult::Failed&&Bits(path)==0,"failed journal write leaves file visible and intact");
    Check(HideDesktopItem(path)==DesktopManageResult::Failed&&Bits(path)==0,"repeated acquisition cannot bypass failed journal write");
    Check(RemoveDirectoryW(blocker.c_str())!=FALSE,"remove journal fixture blocker");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"journal acquisition succeeds after write failure is resolved");
    const auto primary=data+L"\\hidden-items.dat",backup=data+L"\\hidden-items.backup.dat",safe=data+L"\\safe.dat";
    Check(CopyFileW(primary.c_str(),safe.c_str(),TRUE)!=FALSE,"save only fixture metadata for journal recovery");ClearHiddenItems();Corrupt(primary);
    Check(testing::InitializeHiddenItemsAt(data,desktop,common)&&RestoreHiddenItems()&&Bits(path)==0,"backup journal recovers from primary CRC/format failure");
    Check(HideDesktopItem(path)==DesktopManageResult::Managed,"hide for double-corrupt journal test");ClearHiddenItems();Corrupt(primary);Corrupt(backup);
    Check(!testing::InitializeHiddenItemsAt(data,desktop,common)&&HideDesktopItem(folder)==DesktopManageResult::Failed&&Content(path)&&Bits(folder)==0,
        "two corrupt journals fail closed without moving/deleting files or acquiring another item");
    Check(CopyFileW(safe.c_str(),primary.c_str(),FALSE)!=FALSE&&testing::InitializeHiddenItemsAt(data,desktop,common)&&RestoreHiddenItems()&&Bits(path)==0,
        "restoring valid fixture metadata safely recovers visibility");DeleteFileW(safe.c_str());
    ClearHiddenItems();
    const auto executable=ExecutablePath();std::wstring command=L"\""+executable+L"\" /abrupt-fixture \""+data+L"\" \""+desktop+L"\" \""+common+L"\" \""+path+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    const bool launched=CreateProcessW(executable.c_str(),&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    Check(launched,"launch isolated abrupt-exit fixture with separate recovery process");
    if(launched){CloseHandle(process.hThread);const DWORD wait=WaitForSingleObject(process.hProcess,15000);DWORD exit=0;GetExitCodeProcess(process.hProcess,&exit);CloseHandle(process.hProcess);
        Check(wait==WAIT_OBJECT_0&&exit==77,"fixture exited abruptly only after watcher readiness handshake");
        for(int i=0;i<300&&Bits(path)!=0;++i)Sleep(10);
        Check(Bits(path)==0&&Content(path),"independent watcher restores original path visibility after abrupt process exit");
    }
    for(const auto& f:{path,child,outside})Check(DeleteFileW(f.c_str())!=FALSE,"remove explicit isolated file");
    for(const auto* name:{L"hidden-items.dat",L"hidden-items.backup.dat",L"hidden-items.dat.tmp",L"hidden-items.backup.dat.tmp"})DeleteFileW((data+L"\\"+name).c_str());
    for(const auto& dir:{folder,common,desktop,data,root})Check(RemoveDirectoryW(dir.c_str())!=FALSE,"remove empty isolated directories");
    if(!failures)std::puts("All in-place visibility and identity recovery tests passed.");
    return failures?1:0;
}
