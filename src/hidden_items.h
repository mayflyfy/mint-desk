#pragma once
#include "desktop_items.h"

namespace desk {
bool InitializeHiddenItems();
DesktopManageResult HideDesktopItem(const std::wstring& path);
bool IsHiddenDesktopItem(const std::wstring& path);
bool ReleaseHiddenDesktopItem(const std::wstring& path);
bool RestoreHiddenItems();
bool HiddenItemsNeedPermission();
void ClearHiddenItems();
int RunVisibilityWatcher(DWORD parentPid);
namespace testing {
bool InitializeHiddenItemsAt(const std::wstring& data,const std::wstring& desktop,const std::wstring& common);
bool RecoverHiddenItemsAt(const std::wstring& data,const std::wstring& desktop,const std::wstring& common,DWORD owner);
bool StartHiddenItemsWatcherAt();
int RunVisibilityWatcherAt(DWORD parentPid,const std::wstring& data,const std::wstring& desktop,const std::wstring& common);
}
}
