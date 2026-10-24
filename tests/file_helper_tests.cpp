#include "../src/file_helper.h"
#include <cstdio>
#include <vector>
#include <cstring>

namespace {
int failures=0;
void Check(bool value,const char* name){if(!value){std::fprintf(stderr,"FAIL: %s\n",name);++failures;}}
void Write(const std::wstring& p,const char* value){HANDLE f=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);Check(f!=INVALID_HANDLE_VALUE,"create isolated payload");if(f==INVALID_HANDLE_VALUE)return;DWORD n=0;Check(WriteFile(f,value,static_cast<DWORD>(strlen(value)),&n,nullptr)!=FALSE,"write isolated payload");CloseHandle(f);}
std::string Read(const std::wstring& p){HANDLE f=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(f==INVALID_HANDLE_VALUE)return {};char b[64]{};DWORD n=0;ReadFile(f,b,64,&n,nullptr);CloseHandle(f);return {b,n};}
}
int main() {
    using namespace desk;
    const std::wstring desktop=L"C:\\Users\\Test User\\Desktop",common=L"C:\\Users\\Public\\Desktop",
        managed=L"C:\\Users\\Test User\\AppData\\Local\\MintDesk\\Managed";
    auto allowed=[&](const std::wstring& a,const std::wstring& b){return testing::IsAllowedDesktopMove(desktop,common,managed,a,b);};
    Check(allowed(common+L"\\App.lnk",managed+L"\\123-0\\App.lnk"),"public desktop import stays within an allocated Managed slot");
    Check(allowed(desktop+L"\\File.txt",managed+L"\\123-1\\File.txt"),"private desktop import is supported");
    Check(allowed(managed+L"\\123-0\\App.lnk",common+L"\\App.lnk"),"public restore is allowed");
    Check(allowed(managed+L"\\123-0\\App.lnk",common+L"\\App (MintDesk 1).lnk"),"collision-safe restoration may use a unique name");
    Check(allowed(L"c:\\users\\PUBLIC\\Desktop\\App.lnk",managed+L"\\123-0\\App.lnk"),"Windows roots compare without case sensitivity");
    for(const auto& bad:std::vector<std::wstring>{L"C:\\Windows\\System32\\file.dll",L"C:\\Users\\Public\\Other\\App.lnk",
        common+L"\\Folder\\App.lnk",common+L"\\..\\Other\\App.lnk",L"relative.txt",L"\\\\server\\share\\App.lnk",
        L"\\\\?\\C:\\Users\\Public\\Desktop\\App.lnk",common+L"\\App.lnk:stream",common+L"\\NUL",common+L"\\COM1.txt",
        common+L"\\App.lnk.",common+L"\\App.lnk ",common+L"\\*.lnk",common+L"\\App\n.lnk"})
        Check(!allowed(bad,managed+L"\\123-0\\App.lnk"),"restricted helper rejects paths outside its exact desktop operation");
    for(const auto& bad:std::vector<std::wstring>{managed+L"\\App.lnk",managed+L"\\slot\\App.lnk",managed+L"\\123--0\\App.lnk",
        managed+L"\\-0\\App.lnk",managed+L"\\123-\\App.lnk",managed+L"\\123-0\\Other.lnk",
        managed+L"\\123-0\\Folder\\App.lnk",managed+L"-evil\\123-0\\App.lnk",L"C:\\Windows\\App.lnk"})
        Check(!allowed(common+L"\\App.lnk",bad),"import cannot write an arbitrary destination or change the original basename");
    Check(!allowed(managed+L"\\123-0\\App.lnk",L"C:\\Windows\\App.lnk"),"restore cannot write outside desktop");
    Check(!FileHelperRunning(),"unit tests start without elevation");
    Check(PrivilegedDesktopMove(common+L"\\App.lnk",managed+L"\\123-0\\App.lnk")==ERROR_PRIVILEGE_NOT_HELD,
        "an absent helper cannot perform a privileged move");
    Check(CheckPrivilegedDesktopRestore(common)==ERROR_PRIVILEGE_NOT_HELD,"permission probe does not launch or alter system settings");
    Check(RunFileHelper(0,L"0123456789abcdef0123456789abcdef")==1&&RunFileHelper(GetCurrentProcessId(),L"bad")==1,
        "invalid helper entry cannot connect to any parent");
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const std::wstring root=std::wstring(temp)+L"MintDesk-helper-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto d=root+L"\\Desktop",p=root+L"\\Public",m=root+L"\\Managed",slot=m+L"\\123-0";
    for(const auto& dir:{root,d,p,m,slot})Check(CreateDirectoryW(dir.c_str(),nullptr)!=FALSE,"create isolated helper roots");
    const auto source=p+L"\\payload.txt",destination=slot+L"\\payload.txt";
    Write(source,"original bytes");
    Check(testing::MoveDesktopItemAt(d,p,m,source,destination)==ERROR_SUCCESS&&Read(destination)=="original bytes"&&GetFileAttributesW(source.c_str())==INVALID_FILE_ATTRIBUTES,
        "pinned ancestor handles permit atomic import and preserve exact data");
    Write(source,"unrelated replacement");
    const auto collision=testing::MoveDesktopItemAt(d,p,m,destination,source);
    Check((collision==ERROR_ALREADY_EXISTS||collision==ERROR_FILE_EXISTS)&&Read(source)=="unrelated replacement"&&Read(destination)=="original bytes",
        "privileged rename never overwrites a concurrently created desktop target");
    const auto alternate=p+L"\\payload (MintDesk 1).txt";
    Check(testing::MoveDesktopItemAt(d,p,m,destination,alternate)==ERROR_SUCCESS&&Read(alternate)=="original bytes","restore to a unique name preserves the original payload");
    for(const auto& f:{source,alternate})Check(DeleteFileW(f.c_str())!=FALSE,"remove only explicitly created test files");
    for(const auto& dir:{slot,m,p,d,root})Check(RemoveDirectoryW(dir.c_str())!=FALSE,"remove empty test directories");
    StopFileHelper();
    if(!failures)std::puts("All file helper boundary tests passed (no elevation requested).");
    return failures?1:0;
}
