// Test-only installer payload. Never built into the app or release packages.
#include "../src/startup_integration.h"
#include "../src/desktop_menu.h"
#include "../src/state.h"
#include <shlobj.h>
#include <string>

namespace {
constexpr wchar_t kPreferences[]=L"Software\\MCC\\MintDeskInstallerTests";
struct Context {HKEY run=nullptr,shell=nullptr;std::wstring desktop;};
bool Start(void* pointer,bool enabled) {
    auto& context=*static_cast<Context*>(pointer);
    if(!enabled){const LONG error=RegDeleteValueW(context.run,L"MintDesk");return error==ERROR_SUCCESS||error==ERROR_FILE_NOT_FOUND;}
    const auto command=L"\""+desk::ExecutablePath()+L"\" /background";
    return RegSetValueExW(context.run,L"MintDesk",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
}
bool Menu(void* pointer,bool enabled) {
    return desk::testing::SetDesktopMenuAtKey(static_cast<Context*>(pointer)->shell,desk::ExecutablePath(),enabled,false);
}
bool Shortcut(void* pointer,bool enabled) {
    return desk::testing::SetDesktopShortcutAt(static_cast<Context*>(pointer)->desktop,desk::ExecutablePath(),enabled);
}
}
int wmain(int argc,wchar_t** argv) {
    HKEY preferences=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,kPreferences,0,KEY_ALL_ACCESS,&preferences)!=ERROR_SUCCESS)return 1;
    if(argc==2&&!wcscmp(argv[1],L"/prepare-install")) {
        DWORD value=0,size=sizeof(value);RegQueryValueExW(preferences,L"PrepareResult",nullptr,nullptr,reinterpret_cast<BYTE*>(&value),&size);
        RegCloseKey(preferences);return static_cast<int>(value);
    }
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Context context;
    wchar_t desktop[32768]{};DWORD bytes=sizeof(desktop);
    const bool ready=RegGetValueW(preferences,nullptr,L"FixtureDesktop",RRF_RT_REG_SZ,nullptr,desktop,&bytes)==ERROR_SUCCESS&&
        RegCreateKeyExW(preferences,L"Run",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&context.run,nullptr)==ERROR_SUCCESS&&
        RegCreateKeyExW(preferences,L"Shell",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&context.shell,nullptr)==ERROR_SUCCESS;
    context.desktop=desktop;bool okay=false;
    if(ready&&argc==6&&!wcscmp(argv[1],L"/configure-install")) {
        desk::InstallOptions options;options.desktopShortcut=argv[2][0]==L'1';options.autostart=argv[3][0]==L'1';
        options.autoCheckUpdates=argv[4][0]==L'1';options.desktopMenu=argv[5][0]==L'1';
        okay=desk::testing::ApplyInstallOptionsAtKey(preferences,options,{&context,Start,Menu,nullptr,nullptr,Shortcut});
    } else if(ready&&argc==2&&!wcscmp(argv[1],L"/uninstall-integrations")) {
        okay=Start(&context,false)&&Menu(&context,false)&&
            desk::testing::SetDesktopShortcutAt(context.desktop,desk::ExecutablePath(),false,true);
    }
    if(context.run)RegCloseKey(context.run);
    if(context.shell)RegCloseKey(context.shell);
    RegCloseKey(preferences);if(SUCCEEDED(com))CoUninitialize();return okay?0:1;
}
