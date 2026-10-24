#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace desk {

enum class DesktopManageResult { Reference, Managed, Failed, RequiresPermission };

// Desktop entries stay at their original paths with journaled visibility bits.
// Legacy moves are recovered before acquiring anything. No payload is deleted.
bool InitializeDesktopItems();
DesktopManageResult ManageDesktopItem(const std::wstring& reference);
std::wstring ResolveDesktopItem(const std::wstring& reference);
// Convert paths supplied by native Shell drags back to persistent card identity.
std::wstring ReferenceForDesktopItem(const std::wstring& actualPath);
// True only when this exact reference owns a managed entry. Nested references
// resolve through a managed folder without acquiring ownership of that folder.
bool IsManagedDesktopItem(const std::wstring& reference);
bool ReleaseDesktopItem(const std::wstring& reference);
bool RestoreDesktopItems();
// True after a failed restore that can be retried with the file-only helper.
bool DesktopItemsNeedPermission();
// Clears only in-memory state; it never deletes the journal or managed files.
void ClearDesktopItems();

namespace testing {
// Explicit test injection, never read from environment variables or switches.
bool InitializeDesktopItemsAt(const std::wstring& directory,
    const std::wstring& userDesktop, const std::wstring& publicDesktop,
    const std::wstring& managedDirectory);
}

} // namespace desk
