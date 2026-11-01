#include "startup_integration.h"
#include "state.h"
#include "desktop_menu.h"
#include <shlobj.h>
#include <cstring>

namespace desk {
namespace {
constexpr wchar_t kPreferences[]=L"Software\\MCC\\MintDesk";
constexpr wchar_t kCompleted[]=L"InitialIntegrationV1";
constexpr wchar_t kDesktopShortcut[]=L"DesktopShortcutEnabled";
constexpr wchar_t kAutoUpdate[]=L"AutoCheckUpdates";
bool ReadBool(HKEY key,const wchar_t* name,bool fallback,bool& value) {
    DWORD number=0,type=0,size=sizeof(number);
    const LONG result=RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(&number),&size);
    if(result==ERROR_FILE_NOT_FOUND){value=fallback;return true;}
    if(result!=ERROR_SUCCESS||type!=REG_DWORD||size!=sizeof(number)||number>1)return false;
    value=number!=0;return true;
}
bool WriteNumber(HKEY key,const wchar_t* name,DWORD value) {
    return RegSetValueExW(key,name,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS;
}
bool Configure(HKEY key,const testing::IntegrationCallbacks& callbacks) {
    if(!key||!callbacks.autostart||!callbacks.desktopMenu)return false;
    bool shortcut=true;
    if(!ReadBool(key,kDesktopShortcut,true,shortcut))return false;
    DWORD completed=0,type=0,size=sizeof(completed);
    const LONG read=RegQueryValueExW(key,kCompleted,nullptr,&type,reinterpret_cast<BYTE*>(&completed),&size);
    if(read!=ERROR_FILE_NOT_FOUND&&(read!=ERROR_SUCCESS||type!=REG_DWORD||size!=sizeof(completed)||(completed&~3u)))return false;
    auto save=[&](){return RegSetValueExW(key,kCompleted,0,REG_DWORD,reinterpret_cast<const BYTE*>(&completed),sizeof(completed))==ERROR_SUCCESS;};
    bool success=true;
    if(!(completed&1u)) {
        if(callbacks.autostart(callbacks.context,true)){completed|=1u;if(!save())return false;}
        else success=false;
    } else if(callbacks.autostartNeedsRefresh&&callbacks.autostartNeedsRefresh(callbacks.context)) {
        if(!callbacks.autostart(callbacks.context,true))success=false;
    }
    if(!(completed&2u)) {
        if(callbacks.desktopMenu(callbacks.context,true)){completed|=2u;if(!save())return false;}
        else success=false;
    } else if(callbacks.desktopMenuNeedsRefresh&&callbacks.desktopMenuNeedsRefresh(callbacks.context)) {
        if(!callbacks.desktopMenu(callbacks.context,true))success=false;
    }
    if(callbacks.desktopShortcut&&!callbacks.desktopShortcut(callbacks.context,shortcut))success=false;
    return success;
}
bool Apply(HKEY key,const InstallOptions& options,const testing::IntegrationCallbacks& callbacks) {
    if(!key||!callbacks.autostart||!callbacks.desktopMenu||!callbacks.desktopShortcut)return false;
    // Persist explicit choices before applying them so a later normal launch
    // cannot replace an installer's opt-out with the portable first-run defaults.
    if(!WriteNumber(key,kDesktopShortcut,options.desktopShortcut)||
        !WriteNumber(key,kAutoUpdate,options.autoCheckUpdates)||!WriteNumber(key,kCompleted,3))return false;
    const bool shortcut=callbacks.desktopShortcut(callbacks.context,options.desktopShortcut);
    const bool startup=callbacks.autostart(callbacks.context,options.autostart);
    const bool menu=callbacks.desktopMenu(callbacks.context,options.desktopMenu);
    return shortcut&&startup&&menu;
}
bool Autostart(void*,bool enabled){return SetAutostart(enabled);}
bool DesktopMenu(void*,bool enabled){return SetDesktopMenu(enabled);}
bool AutostartNeedsRefresh(void*) {
    DWORD size=0;
    const bool exists=RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"MintDesk",RRF_RT_REG_SZ,nullptr,nullptr,&size)==ERROR_SUCCESS&&size>sizeof(wchar_t);
    return exists&&!IsAutostartEnabled();
}
bool DesktopMenuNeedsRefresh(void*) {
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Classes\\DesktopBackground\\Shell\\MintDesk",0,KEY_READ,&key)!=ERROR_SUCCESS)return false;
    RegCloseKey(key);return !IsDesktopMenuRegistered();
}

template<class T> struct Com {
    T* value=nullptr;
    ~Com(){if(value)value->Release();}
};
constexpr wchar_t kShortcutDescription[]=L"薄荷桌面助手 / MintDesk";
bool ShortcutAt(const std::wstring& desktop,const std::wstring& executable,bool enabled=true,bool matchingTargetOnly=false) {
    if(desktop.empty()||executable.empty())return false;
    Com<IShellLinkW> desired;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,
        reinterpret_cast<void**>(&desired.value))))return false;
    const auto slash=executable.find_last_of(L"\\/");
    if(FAILED(desired.value->SetPath(executable.c_str())) ||
        FAILED(desired.value->SetArguments(L"/show")) ||
        FAILED(desired.value->SetDescription(kShortcutDescription)) ||
        FAILED(desired.value->SetIconLocation(executable.c_str(),0)) ||
        (slash!=std::wstring::npos && FAILED(desired.value->SetWorkingDirectory(executable.substr(0,slash).c_str()))))return false;
    for(const auto* name:{L"薄荷桌面助手.lnk",L"MintDesk.lnk"}) {
        const auto path=desktop+L"\\"+name;
        const DWORD attributes=GetFileAttributesW(path.c_str());
        const bool exists=attributes!=INVALID_FILE_ATTRIBUTES;
        if(exists) {
            if(attributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            Com<IShellLinkW> existing;Com<IPersistFile> file;
            if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,
                reinterpret_cast<void**>(&existing.value))) ||
                FAILED(existing.value->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file.value))) ||
                FAILED(file.value->Load(path.c_str(),STGM_READ)))continue;
            wchar_t target[32768]{},description[256]{},arguments[256]{};
            if(FAILED(existing.value->GetPath(target,ARRAYSIZE(target),nullptr,SLGP_RAWPATH)) ||
                FAILED(existing.value->GetDescription(description,ARRAYSIZE(description))) ||
                FAILED(existing.value->GetArguments(arguments,ARRAYSIZE(arguments))))continue;
            const bool ours=!wcscmp(description,kShortcutDescription);
            if(!enabled) {
                if(ours&&(!matchingTargetOnly||!_wcsicmp(target,executable.c_str()))) {
                    if(!DeleteFileW(path.c_str()))return false;
                    SHChangeNotify(SHCNE_DELETE,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,path.c_str(),nullptr);
                }
                continue;
            }
            if(!_wcsicmp(target,executable.c_str()) && (!ours || !wcscmp(arguments,L"/show")))return true;
            if(!ours)continue; // Never replace another application's or user's shortcut.
        }
        if(!enabled)continue;
        // Serialize in memory. Updating our existing shortcut in place keeps
        // its file identity and Hidden/System flags if it was put in a card.
        Com<IPersistStream> persistent;Com<IStream> stream;
        if(FAILED(desired.value->QueryInterface(IID_IPersistStream,reinterpret_cast<void**>(&persistent.value))) ||
            FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream.value)) ||
            FAILED(persistent.value->Save(stream.value,TRUE)))return false;
        STATSTG stat{};HGLOBAL storage=nullptr;
        if(FAILED(stream.value->Stat(&stat,STATFLAG_NONAME)) || stat.cbSize.HighPart ||
            !stat.cbSize.LowPart || stat.cbSize.LowPart>1024*1024 ||
            FAILED(GetHGlobalFromStream(stream.value,&storage)))return false;
        const void* bytes=GlobalLock(storage);if(!bytes)return false;
        HANDLE output=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,exists?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        DWORD written=0;
        const bool success=output!=INVALID_HANDLE_VALUE &&
            WriteFile(output,bytes,stat.cbSize.LowPart,&written,nullptr) && written==stat.cbSize.LowPart &&
            SetEndOfFile(output) && FlushFileBuffers(output);
        if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
        GlobalUnlock(storage);
        if(success)SHChangeNotify(exists?SHCNE_UPDATEITEM:SHCNE_CREATE,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,path.c_str(),nullptr);
        return success;
    }
    return !enabled;
}
bool DesktopShortcut(void*,bool enabled) {
    PWSTR desktop=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop)))return false;
    const std::wstring directory=desktop;CoTaskMemFree(desktop);
    return ShortcutAt(directory,ExecutablePath(),enabled);
}
testing::IntegrationCallbacks Callbacks(){return {nullptr,Autostart,DesktopMenu,AutostartNeedsRefresh,DesktopMenuNeedsRefresh,DesktopShortcut};}
}
bool EnsureDefaultIntegrations() {
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,kPreferences,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    bool success=false;
    try{success=Configure(key,Callbacks());}catch(...){success=false;}
    RegCloseKey(key);return success;
}
bool ApplyInstallOptions(const InstallOptions& options) {
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,kPreferences,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    bool success=false;
    try{success=Apply(key,options,Callbacks());}catch(...){success=false;}
    RegCloseKey(key);return success;
}
bool IsAutoCheckUpdatesEnabled() {
    HKEY key=nullptr;bool value=false;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,kPreferences,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;
    const bool okay=ReadBool(key,kAutoUpdate,false,value);RegCloseKey(key);return okay&&value;
}
bool RemoveInstalledIntegrations() {
    bool okay=true;
    // A stale copy must not remove entries that now point to another location.
    if(IsAutostartEnabled()&&!SetAutostart(false))okay=false;
    if(IsDesktopMenuRegistered()&&!SetDesktopMenu(false))okay=false;
    PWSTR desktop=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop)))return false;
    try{if(!ShortcutAt(desktop,ExecutablePath(),false,true))okay=false;}catch(...){okay=false;}
    CoTaskMemFree(desktop);return okay;
}
namespace testing {
bool EnsureDefaultIntegrationsAtKey(HKEY key,const IntegrationCallbacks& callbacks){return Configure(key,callbacks);}
bool EnsureDesktopShortcutAt(const std::wstring& desktop,const std::wstring& executable){return ShortcutAt(desktop,executable);}
bool SetDesktopShortcutAt(const std::wstring& desktop,const std::wstring& executable,bool enabled,bool matchingTargetOnly){return ShortcutAt(desktop,executable,enabled,matchingTargetOnly);}
bool ApplyInstallOptionsAtKey(HKEY key,const InstallOptions& options,const IntegrationCallbacks& callbacks){return Apply(key,options,callbacks);}
}
}
