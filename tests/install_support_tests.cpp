#include "../src/install_support.h"
#include <cstdio>
#include <string>

namespace {
int failures=0;
void Check(bool value,const char* label){if(!value){std::fprintf(stderr,"FAIL: %s\n",label);++failures;}}
constexpr wchar_t kClass[]=L"MintDesk.MaintenanceFixture.1";
bool blockClose=false;
LRESULT CALLBACK Fixture(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_CLOSE){if(!blockClose)DestroyWindow(hwnd);return 0;}
    if(message==WM_APP+1){blockClose=false;DestroyWindow(hwnd);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==4&&!wcscmp(argv[1],L"/fixture")) {
        blockClose=!wcscmp(argv[3],L"1");
        WNDCLASSW wc{};wc.lpfnWndProc=Fixture;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kClass;
        if(!RegisterClassW(&wc))return 2;
        if(!CreateWindowW(kClass,argv[2],WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr))return 2;
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0)DispatchMessageW(&message);
        return 0;
    }
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
    for(bool blocked:{false,true}) {
        const auto title=L"Isolated maintenance fixture "+std::to_wstring(GetCurrentProcessId());
        std::wstring command=L"\""+std::wstring(executable)+L"\" /fixture \""+title+L"\" "+(blocked?L"1":L"0");
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        Check(CreateProcessW(executable,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"launch synthetic maintenance controller");
        if(!process.hProcess)continue;
        HWND window=nullptr;const ULONGLONG deadline=GetTickCount64()+5000;
        while(!(window=FindWindowW(kClass,title.c_str()))&&GetTickCount64()<deadline)Sleep(10);
        Check(window!=nullptr,"synthetic controller ready");
        const bool result=window&&desk::testing::CloseControllerForMaintenance(kClass,title.c_str(),blocked?100:5000);
        Check(result==!blocked,"maintenance waits for normal exit and rejects an unfinished restoration");
        if(blocked) {
            DWORD code=0;
            Check(GetExitCodeProcess(process.hProcess,&code)&&code==STILL_ACTIVE,"timeout never force-terminates a blocked application");
            PostMessageW(window,WM_APP+1,0,0);
        }
        Check(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0,"fixture exits normally after test cleanup");
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
    }
    Check(desk::testing::CloseControllerForMaintenance(kClass,L"absent fixture",50),"absent instance needs no forced cleanup");
    if(!failures)std::puts("Maintenance tests passed (isolated controller processes only).");
    return failures?1:0;
}
