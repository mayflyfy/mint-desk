#pragma once
#include <windows.h>

namespace desk {
// Normal WM_CLOSE only. Never terminates a process or removes recovery data.
bool CloseForMaintenance(DWORD timeoutMs=30000);
namespace testing {
bool CloseControllerForMaintenance(const wchar_t* windowClass,const wchar_t* title,DWORD timeoutMs);
}
}
