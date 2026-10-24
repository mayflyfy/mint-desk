#pragma once
#include <windows.h>
#include <string>
#include "visibility.h"

namespace desk {
// The normal-integrity UI retains OLE drag/drop. Only constrained same-volume
// desktop <-> Managed renames are sent to the explicitly elevated helper.
bool EnsureFileHelper(HWND owner = nullptr, bool retryConsent = false);
bool FileHelperRunning();
// A timed-out rename must settle before its journal can be reconciled.
bool FileHelperOperationPending();
bool WaitForFileMovesIdle();
DWORD CheckPrivilegedDesktopRestore(const std::wstring& desktop);
DWORD PrivilegedDesktopMove(const std::wstring& source, const std::wstring& target);
DWORD PrivilegedDesktopVisibility(const std::wstring& path,const FileIdentity& expected,DWORD bits);
void StopFileHelper();
int RunFileHelper(DWORD parentProcess, const std::wstring& nonce);
namespace testing {
bool IsAllowedDesktopMove(const std::wstring& desktop, const std::wstring& common,
    const std::wstring& managed, const std::wstring& source, const std::wstring& target);
DWORD MoveDesktopItemAt(const std::wstring& desktop, const std::wstring& common,
    const std::wstring& managed, const std::wstring& source, const std::wstring& target);
DWORD DesktopVisibilityAt(const std::wstring& desktop,const std::wstring& common,
    const std::wstring& path,const FileIdentity& expected,DWORD bits);
}
}
