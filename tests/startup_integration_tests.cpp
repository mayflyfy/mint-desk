#include "../src/startup_integration.h"
#include <cstdio>
#include <string>
#include <cstring>
#include <shlobj.h>

namespace {
int failures=0;
void Check(bool condition,const char* text){if(!condition){std::fprintf(stderr,"FAIL: %s\n",text);++failures;}}
struct Calls {int start=0,menu=0;bool startOk=true,menuOk=true,startEnabled=false,menuEnabled=false,startStale=false,menuStale=false;};
bool Start(void* context,bool enabled){auto& c=*static_cast<Calls*>(context);++c.start;if(c.startOk)c.startEnabled=enabled;return c.startOk;}
bool Menu(void* context,bool enabled){auto& c=*static_cast<Calls*>(context);++c.menu;if(c.menuOk)c.menuEnabled=enabled;return c.menuOk;}
bool StartStale(void* context){return static_cast<Calls*>(context)->startStale;}
bool MenuStale(void* context){return static_cast<Calls*>(context)->menuStale;}
bool ShortcutTarget(const std::wstring& path,const std::wstring& expected) {
    IShellLinkW* link=nullptr;IPersistFile* file=nullptr;bool okay=false;
    if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))) &&
        SUCCEEDED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file))) && SUCCEEDED(file->Load(path.c_str(),STGM_READ))) {
        wchar_t target[32768]{},arguments[64]{};
        okay=SUCCEEDED(link->GetPath(target,ARRAYSIZE(target),nullptr,SLGP_RAWPATH)) && !_wcsicmp(target,expected.c_str()) &&
            SUCCEEDED(link->GetArguments(arguments,ARRAYSIZE(arguments))) && !wcscmp(arguments,L"/show");
    }
    if(file)file->Release();
    if(link)link->Release();
    return okay;
}
BY_HANDLE_FILE_INFORMATION Identity(const std::wstring& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(file!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(file,&info),"read isolated shortcut file identity");
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    return info;
}
void TestDesktopShortcut() {
    wchar_t temporary[MAX_PATH]{};GetTempPathW(MAX_PATH,temporary);
    const auto desktop=std::wstring(temporary)+L"MintDesk-shortcut-test-"+std::to_wstring(GetCurrentProcessId());
    Check(CreateDirectoryW(desktop.c_str(),nullptr)!=FALSE,"create isolated desktop shortcut fixture");
    const auto shortcut=desktop+L"\\薄荷桌面助手.lnk",fallback=desktop+L"\\MintDesk.lnk";
    const auto oldExe=desktop+L"\\version-one\\MintDesk.exe",newExe=desktop+L"\\version-two\\MintDesk.exe";
    Check(desk::testing::EnsureDesktopShortcutAt(desktop,oldExe) && ShortcutTarget(shortcut,oldExe),
        "normal launch creates a real native desktop shortcut to this executable");
    const auto before=Identity(shortcut);
    Check(desk::testing::EnsureDesktopShortcutAt(desktop,oldExe) &&
        GetFileAttributesW(fallback.c_str())==INVALID_FILE_ATTRIBUTES,"reopening creates no duplicate shortcut");
    Check(SetFileAttributesW(shortcut.c_str(),FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)!=FALSE,"simulate shortcut already organized in a card");
    Check(desk::testing::EnsureDesktopShortcutAt(desktop,newExe) && ShortcutTarget(shortcut,newExe),
        "upgrading to another executable directory refreshes the shortcut target");
    const auto after=Identity(shortcut);
    Check(before.dwVolumeSerialNumber==after.dwVolumeSerialNumber && before.nFileIndexHigh==after.nFileIndexHigh &&
        before.nFileIndexLow==after.nFileIndexLow &&
        (GetFileAttributesW(shortcut.c_str())&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))==(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM),
        "refresh preserves shortcut identity and existing organizer visibility attributes");
    SetFileAttributesW(shortcut.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(shortcut.c_str());
    const char original[]="keep this unrelated user file";
    HANDLE file=CreateFileW(shortcut.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);DWORD written=0;
    Check(file!=INVALID_HANDLE_VALUE && WriteFile(file,original,sizeof(original),&written,nullptr),"create unrelated same-name user file");
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    Check(desk::testing::EnsureDesktopShortcutAt(desktop,newExe) && ShortcutTarget(fallback,newExe),
        "same-name collision uses a fallback shortcut without overwriting a user file");
    file=CreateFileW(shortcut.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    char bytes[sizeof(original)]{};DWORD read=0;
    Check(file!=INVALID_HANDLE_VALUE && ReadFile(file,bytes,sizeof(bytes),&read,nullptr) && read==sizeof(original) &&
        !memcmp(bytes,original,sizeof(original)),"unrelated same-name file contents remain intact");
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    Check(desk::testing::EnsureDesktopShortcutAt(desktop,newExe),"collision fallback also remains stable across launches");
    DeleteFileW(shortcut.c_str());DeleteFileW(fallback.c_str());
    Check(RemoveDirectoryW(desktop.c_str())!=FALSE,"remove only explicit isolated shortcut fixtures");
}
}
int main() {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const auto path=L"Software\\MintDeskIntegrationTests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    HKEY key=nullptr;Check(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr)==ERROR_SUCCESS,"create isolated preference key");
    if(!key)return 1;
    Calls calls;desk::testing::IntegrationCallbacks callbacks{&calls,Start,Menu};
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==1&&calls.startEnabled&&calls.menuEnabled,
        "first normal startup enables both registrations");
    calls.startEnabled=false;calls.menuEnabled=false;
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==1&&!calls.startEnabled&&!calls.menuEnabled,
        "later launches preserve manual opt-outs instead of re-enabling them");
    callbacks.autostartNeedsRefresh=StartStale;callbacks.desktopMenuNeedsRefresh=MenuStale;
    calls.startStale=true;calls.menuStale=true;
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==2&&calls.menu==2&&calls.startEnabled&&calls.menuEnabled,
        "launching an executable in a new directory refreshes existing enabled integrations");
    calls.startStale=false;calls.menuStale=false;calls.startEnabled=false;calls.menuEnabled=false;
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==2&&calls.menu==2&&!calls.startEnabled&&!calls.menuEnabled,
        "path refresh does not recreate manually removed integration entries");
    RegDeleteValueW(key,L"InitialIntegrationV1");calls={};calls.menuOk=false;
    Check(!desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==1,"partial setup reports failure");
    calls.startEnabled=false;calls.menuOk=true;
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==2&&!calls.startEnabled&&calls.menuEnabled,
        "retry performs only the unfinished setting and preserves an intervening opt-out");
    const wchar_t malformed[]=L"invalid";
    RegSetValueExW(key,L"InitialIntegrationV1",0,REG_SZ,reinterpret_cast<const BYTE*>(malformed),sizeof(malformed));
    const int previous=calls.start+calls.menu;
    Check(!desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start+calls.menu==previous,"malformed preferences do not overwrite current settings");
    RegCloseKey(key);Check(RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str())==ERROR_SUCCESS,"remove only isolated preference fixture");
    TestDesktopShortcut();
    if(SUCCEEDED(com))CoUninitialize();
    if(!failures)std::puts("Startup and shortcut tests passed (only isolated fixture integrations changed).");
    return failures?1:0;
}
