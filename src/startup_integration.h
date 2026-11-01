#pragma once
#include <windows.h>
#include <string>

namespace desk {
// Applies defaults once, only for normal UI startup. Later manual opt-outs
// remain disabled because completion is separate from the registration state.
bool EnsureDefaultIntegrations();
struct InstallOptions {
    bool desktopShortcut=true;
    bool autostart=true;
    bool autoCheckUpdates=false; // Preference only; no updater/network activity yet.
    bool desktopMenu=true;
};
bool ApplyInstallOptions(const InstallOptions&);
bool RemoveInstalledIntegrations();
bool IsAutoCheckUpdatesEnabled();
namespace testing {
struct IntegrationCallbacks {
    void* context=nullptr;
    bool (*autostart)(void*,bool)=nullptr;
    bool (*desktopMenu)(void*,bool)=nullptr;
    bool (*autostartNeedsRefresh)(void*)=nullptr;
    bool (*desktopMenuNeedsRefresh)(void*)=nullptr;
    bool (*desktopShortcut)(void*,bool)=nullptr;
};
bool EnsureDefaultIntegrationsAtKey(HKEY preferences,const IntegrationCallbacks& callbacks);
bool EnsureDesktopShortcutAt(const std::wstring& desktop,const std::wstring& executable);
bool SetDesktopShortcutAt(const std::wstring& desktop,const std::wstring& executable,bool enabled,bool matchingTargetOnly=false);
bool ApplyInstallOptionsAtKey(HKEY preferences,const InstallOptions&,const IntegrationCallbacks&);
}
}
