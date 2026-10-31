#pragma once
#include <windows.h>
#include <string>

namespace desk {
// Applies defaults once, only for normal UI startup. Later manual opt-outs
// remain disabled because completion is separate from the registration state.
bool EnsureDefaultIntegrations();
namespace testing {
struct IntegrationCallbacks {
    void* context=nullptr;
    bool (*autostart)(void*,bool)=nullptr;
    bool (*desktopMenu)(void*,bool)=nullptr;
    bool (*autostartNeedsRefresh)(void*)=nullptr;
    bool (*desktopMenuNeedsRefresh)(void*)=nullptr;
};
bool EnsureDefaultIntegrationsAtKey(HKEY preferences,const IntegrationCallbacks& callbacks);
bool EnsureDesktopShortcutAt(const std::wstring& desktop,const std::wstring& executable);
}
}
