#include "../src/startup_integration.h"
#include <cstdio>
#include <string>

namespace {
int failures=0;
void Check(bool condition,const char* text){if(!condition){std::fprintf(stderr,"FAIL: %s\n",text);++failures;}}
struct Calls {int start=0,menu=0;bool startOk=true,menuOk=true,startEnabled=false,menuEnabled=false;};
bool Start(void* context,bool enabled){auto& c=*static_cast<Calls*>(context);++c.start;if(c.startOk)c.startEnabled=enabled;return c.startOk;}
bool Menu(void* context,bool enabled){auto& c=*static_cast<Calls*>(context);++c.menu;if(c.menuOk)c.menuEnabled=enabled;return c.menuOk;}
}
int main() {
    const auto path=L"Software\\MintDeskIntegrationTests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    HKEY key=nullptr;Check(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr)==ERROR_SUCCESS,"create isolated preference key");
    if(!key)return 1;
    Calls calls;desk::testing::IntegrationCallbacks callbacks{&calls,Start,Menu};
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==1&&calls.startEnabled&&calls.menuEnabled,
        "first normal startup enables both registrations");
    calls.startEnabled=false;calls.menuEnabled=false;
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==1&&!calls.startEnabled&&!calls.menuEnabled,
        "later launches preserve manual opt-outs instead of re-enabling them");
    RegDeleteValueW(key,L"InitialIntegrationV1");calls={};calls.menuOk=false;
    Check(!desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==1,"partial setup reports failure");
    calls.startEnabled=false;calls.menuOk=true;
    Check(desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start==1&&calls.menu==2&&!calls.startEnabled&&calls.menuEnabled,
        "retry performs only the unfinished setting and preserves an intervening opt-out");
    const wchar_t malformed[]=L"invalid";
    RegSetValueExW(key,L"InitialIntegrationV1",0,REG_SZ,reinterpret_cast<const BYTE*>(malformed),sizeof(malformed));
    const int previous=calls.start+calls.menu;
    Check(!desk::testing::EnsureDefaultIntegrationsAtKey(key,callbacks)&&calls.start+calls.menu==previous,"malformed preferences do not overwrite current settings");
    RegCloseKey(key);Check(RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str())==ERROR_SUCCESS,"remove only isolated preference fixture");
    if(!failures)std::puts("First-run integration tests passed (no actual startup/menu registration changed).");
    return failures?1:0;
}
