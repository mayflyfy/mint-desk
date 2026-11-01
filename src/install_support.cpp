#include "install_support.h"
#include <tlhelp32.h>
#include <string>
#include <vector>

namespace desk {
namespace {
struct Handles {
    std::vector<HANDLE> values;
    ~Handles(){for(HANDLE value:values)CloseHandle(value);}
};
bool ProcessPath(HANDLE process,std::wstring& path) {
    path.resize(32768);DWORD length=static_cast<DWORD>(path.size());
    if(!QueryFullProcessImageNameW(process,0,&path[0],&length))return false;
    path.resize(length);return true;
}
bool CloseController(const wchar_t* windowClass,const wchar_t* title,DWORD timeoutMs) {
    HWND controller=FindWindowW(windowClass,title);
    if(!controller)return true;
    DWORD pid=0;GetWindowThreadProcessId(controller,&pid);
    if(!pid||pid==GetCurrentProcessId())return false;
    Handles waiting;
    HANDLE main=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!main)return false;
    waiting.values.push_back(main);
    std::wstring executable;if(!ProcessPath(main,executable))return false;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    const auto slash=executable.find_last_of(L"\\/");
    const auto name=executable.substr(slash==std::wstring::npos?0:slash+1);
    DWORD session=0;if(!ProcessIdToSessionId(pid,&session)){CloseHandle(snapshot);return false;}
    bool okay=true;
    for(BOOL next=Process32FirstW(snapshot,&entry);next;next=Process32NextW(snapshot,&entry)) {
        DWORD otherSession=0;
        if(entry.th32ProcessID==pid||entry.th32ProcessID==GetCurrentProcessId()||
            _wcsicmp(entry.szExeFile,name.c_str())||
            !ProcessIdToSessionId(entry.th32ProcessID,&otherSession)||session!=otherSession)continue;
        HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
        if(!process){if(GetLastError()!=ERROR_INVALID_PARAMETER)okay=false;continue;}
        std::wstring path;
        if(!ProcessPath(process,path)){CloseHandle(process);okay=false;continue;}
        if(!_wcsicmp(path.c_str(),executable.c_str()))waiting.values.push_back(process);
        else CloseHandle(process);
    }
    CloseHandle(snapshot);
    if(!okay||!PostMessageW(controller,WM_CLOSE,0,0))return false;
    const ULONGLONG deadline=GetTickCount64()+timeoutMs;
    for(HANDLE process:waiting.values) {
        const ULONGLONG now=GetTickCount64();
        const DWORD remaining=now>=deadline?0:static_cast<DWORD>(deadline-now);
        if(WaitForSingleObject(process,remaining)!=WAIT_OBJECT_0)return false;
    }
    return true;
}
}
bool CloseForMaintenance(DWORD timeoutMs) {
    return CloseController(L"MintDesk.Controller.1",L"MintDesk",timeoutMs)&&
        CloseController(L"DeskCards.Controller.1",L"DeskCards",timeoutMs);
}
namespace testing {
bool CloseControllerForMaintenance(const wchar_t* windowClass,const wchar_t* title,DWORD timeoutMs){return CloseController(windowClass,title,timeoutMs);}
}
}
