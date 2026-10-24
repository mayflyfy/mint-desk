#include "startup_integration.h"
#include "state.h"
#include "desktop_menu.h"

namespace desk {
namespace {
constexpr wchar_t kPreferences[]=L"Software\\MCC\\MintDesk";
constexpr wchar_t kCompleted[]=L"InitialIntegrationV1";
bool Configure(HKEY key,const testing::IntegrationCallbacks& callbacks) {
    if(!key||!callbacks.autostart||!callbacks.desktopMenu)return false;
    DWORD completed=0,type=0,size=sizeof(completed);
    const LONG read=RegQueryValueExW(key,kCompleted,nullptr,&type,reinterpret_cast<BYTE*>(&completed),&size);
    if(read!=ERROR_FILE_NOT_FOUND&&(read!=ERROR_SUCCESS||type!=REG_DWORD||size!=sizeof(completed)||(completed&~3u)))return false;
    auto save=[&](){return RegSetValueExW(key,kCompleted,0,REG_DWORD,reinterpret_cast<const BYTE*>(&completed),sizeof(completed))==ERROR_SUCCESS;};
    bool success=true;
    if(!(completed&1u)) {
        if(callbacks.autostart(callbacks.context,true)){completed|=1u;if(!save())return false;}
        else success=false;
    }
    if(!(completed&2u)) {
        if(callbacks.desktopMenu(callbacks.context,true)){completed|=2u;if(!save())return false;}
        else success=false;
    }
    return success;
}
bool Autostart(void*,bool enabled){return SetAutostart(enabled);}
bool DesktopMenu(void*,bool enabled){return SetDesktopMenu(enabled);}
}
bool EnsureDefaultIntegrations() {
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,kPreferences,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    bool success=false;
    try{success=Configure(key,{nullptr,Autostart,DesktopMenu});}catch(...){success=false;}
    RegCloseKey(key);return success;
}
namespace testing {
bool EnsureDefaultIntegrationsAtKey(HKEY key,const IntegrationCallbacks& callbacks){return Configure(key,callbacks);}
}
}
