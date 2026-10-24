#include "hidden_items.h"
#include "visibility.h"
#include "file_helper.h"
#include "state.h"
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace desk {
namespace {
constexpr BYTE kMagic[8]={'D','S','K','H','I','D','0','1'};
constexpr size_t kLimit=16*1024*1024;
struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit File(HANDLE h):value(h){}
    ~File(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
};
struct Entry {
    std::wstring path;
    FileIdentity id;
    DWORD original=0;
    bool active=false;
    std::unique_ptr<File> handle;
};
struct State {
    std::wstring data,desktop,common;
    std::vector<Entry> entries;
    DWORD owner=0;
    FILETIME born{};
    bool ready=false,injected=false,permission=false;
} g;
SRWLOCK lock=SRWLOCK_INIT;
struct Lock {
    Lock(){AcquireSRWLockExclusive(&lock);}
    ~Lock(){ReleaseSRWLockExclusive(&lock);}
};
struct Gate {
    HANDLE mutex=nullptr;bool held=false;
    Gate(){mutex=CreateMutexW(nullptr,FALSE,L"Local\\DeskCards.Visibility.1");if(mutex){const DWORD w=WaitForSingleObject(mutex,10000);held=w==WAIT_OBJECT_0||w==WAIT_ABANDONED;}}
    ~Gate(){if(held)ReleaseMutex(mutex);if(mutex)CloseHandle(mutex);}
};
bool Equal(const std::wstring& a,const std::wstring& b){return !a.empty()&&!b.empty()&&!_wcsicmp(a.c_str(),b.c_str());}
std::wstring Normalize(const std::wstring& path) {
    if(path.empty()||path.size()>32760||path.find(L'\0')!=std::wstring::npos)return {};
    if(!((path.size()>2&&path[1]==L':')||(path.size()>2&&path[0]==L'\\'&&path[1]==L'\\')))return {};
    if(path.find(L':',2)!=std::wstring::npos)return {};
    std::wstring out(32768,L'\0');const DWORD n=GetFullPathNameW(path.c_str(),static_cast<DWORD>(out.size()),&out[0],nullptr);
    if(!n||n>=out.size())return {};
    out.resize(n);std::replace(out.begin(),out.end(),L'/',L'\\');while(out.size()>3&&out.back()==L'\\')out.pop_back();return out;
}
bool Direct(const std::wstring& path) {
    const auto at=path.find_last_of(L'\\');if(at==std::wstring::npos)return false;
    const auto parent=path.substr(0,at);return Equal(parent,g.desktop)||Equal(parent,g.common);
}
std::wstring Known(REFKNOWNFOLDERID id){PWSTR p=nullptr;if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&p)))return {};const auto result=Normalize(p);CoTaskMemFree(p);return result;}
bool Exists(const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
void Put(std::vector<BYTE>& bytes,DWORD value){for(int i=0;i<4;++i)bytes.push_back(static_cast<BYTE>(value>>(8*i)));}
DWORD Crc(const BYTE* bytes,size_t size){DWORD crc=0xffffffff;for(size_t i=0;i<size;++i){crc^=bytes[i];for(int b=0;b<8;++b)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}return ~crc;}
struct Reader {
    const std::vector<BYTE>& bytes;size_t at=0;
    bool Word(DWORD& n){if(at>bytes.size()||bytes.size()-at<4)return false;n=0;for(int i=0;i<4;++i)n|=DWORD(bytes[at++])<<(8*i);return true;}
    bool Text(std::wstring& text){DWORD n;if(!Word(n)||!n||n>32760||n>(bytes.size()-at)/2)return false;text.clear();for(DWORD i=0;i<n;++i){const wchar_t c=bytes[at]|(bytes[at+1]<<8);at+=2;if(!c)return false;text.push_back(c);}return Normalize(text)==text&&Direct(text);}
};
bool Read(const std::wstring& path) {
    File file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file.value,&size)||size.QuadPart<36||size.QuadPart>static_cast<LONGLONG>(kLimit))return false;
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));DWORD n=0;
    if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)||n!=bytes.size()||std::memcmp(bytes.data(),kMagic,8))return false;
    Reader r{bytes,8};DWORD version,payload,crc,owner,lo,hi,count;
    if(!r.Word(version)||version!=1||!r.Word(payload)||payload!=bytes.size()-20||!r.Word(crc)||crc!=Crc(bytes.data()+20,bytes.size()-20)||
        !r.Word(owner)||!r.Word(lo)||!r.Word(hi)||!r.Word(count)||count>20000)return false;
    std::vector<Entry> entries;entries.reserve(count);
    for(DWORD i=0;i<count;++i){Entry e;DWORD active;
        if(!r.Word(active)||active>1||!r.Word(e.original)||(e.original&~kVisibilityBits)||!r.Word(e.id.volume)||!r.Word(e.id.high)||!r.Word(e.id.low)||!r.Text(e.path))return false;
        if(std::any_of(entries.begin(),entries.end(),[&](const Entry& x){return Equal(x.path,e.path);}))return false;
        e.active=active!=0;entries.push_back(std::move(e));}
    if(r.at!=bytes.size())return false;
    g.entries=std::move(entries);g.owner=owner;g.born={lo,hi};return true;
}
bool Write(const std::wstring& path,const std::vector<BYTE>& bytes) {
    const auto temp=path+L".tmp";
    {File file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_FLAG_WRITE_THROUGH,nullptr));DWORD written=0;
        if(file.value==INVALID_HANDLE_VALUE||!WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.value))return false;}
    return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
bool Persist() {
    std::vector<BYTE> payload;Put(payload,g.owner);Put(payload,g.born.dwLowDateTime);Put(payload,g.born.dwHighDateTime);Put(payload,static_cast<DWORD>(g.entries.size()));
    for(const auto& e:g.entries){Put(payload,e.active?1:0);Put(payload,e.original);Put(payload,e.id.volume);Put(payload,e.id.high);Put(payload,e.id.low);Put(payload,static_cast<DWORD>(e.path.size()));for(wchar_t c:e.path){payload.push_back(static_cast<BYTE>(c));payload.push_back(static_cast<BYTE>(c>>8));}}
    if(payload.size()>kLimit-20)return false;
    std::vector<BYTE> bytes(kMagic,kMagic+8);Put(bytes,1);Put(bytes,static_cast<DWORD>(payload.size()));Put(bytes,Crc(payload.data(),payload.size()));bytes.insert(bytes.end(),payload.begin(),payload.end());
    return Write(g.data+L"\\hidden-items.dat",bytes)&&Write(g.data+L"\\hidden-items.backup.dat",bytes);
}
void Notify(const std::wstring& path,DWORD attributes,bool hiding) {
    SHChangeNotify(SHCNE_ATTRIBUTES,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,path.c_str(),nullptr);
    const bool directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    SHChangeNotify(hiding?(directory?SHCNE_RMDIR:SHCNE_DELETE):(directory?SHCNE_MKDIR:SHCNE_CREATE),SHCNF_PATHW|SHCNF_FLUSHNOWAIT,path.c_str(),nullptr);
}
DWORD Change(Entry& e,DWORD bits) {
    if(!g.injected&&FileHelperOperationPending())return ERROR_IO_INCOMPLETE;
    if(!e.handle||e.handle->value==INVALID_HANDLE_VALUE)e.handle=std::make_unique<File>(OpenVisibility(e.path,true));
    DWORD error=e.handle->value==INVALID_HANDLE_VALUE?GetLastError():ChangeVisibility(e.handle->value,e.id,bits);
    if(error==ERROR_ACCESS_DENIED) {
        File probe(OpenVisibility(e.path,false));FileIdentity id;DWORD attributes=0;
        if(probe.value!=INVALID_HANDLE_VALUE&&VisibilityInfo(probe.value,id,attributes)) {
            if(!(id==e.id))error=ERROR_FILE_INVALID;
            else if((attributes&kVisibilityBits)==bits)error=ERROR_SUCCESS;
        }
    }
    if(error==ERROR_ACCESS_DENIED&&!g.injected&&FileHelperRunning())error=PrivilegedDesktopVisibility(e.path,e.id,bits);
    if(!error){DWORD attributes=GetFileAttributesW(e.path.c_str());const auto path=e.handle->value!=INVALID_HANDLE_VALUE?VisibilityPath(e.handle->value,e.path):e.path;Notify(path,attributes,bits==kVisibilityBits);}
    return error;
}
bool Restore(Entry& e) {
    if(!e.active)return true;
    const DWORD error=Change(e,e.original);
    // No payload is held elsewhere. Never change a replacement at the same
    // path, nor resurrect a file the user has deleted or moved externally.
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_FILE_INVALID) {
        e.active=false;e.handle.reset();return Persist();
    }
    if(error){if(error==ERROR_ACCESS_DENIED)g.permission=true;return false;}
    e.active=false;e.handle.reset();return Persist();
}
bool InitializeAt(const std::wstring& data,const std::wstring& desktop,const std::wstring& common,bool injected,bool claim=true) {
    g={};g.data=Normalize(data);g.desktop=Normalize(desktop);g.common=Normalize(common);g.injected=injected;
    if(g.data.empty()||(g.desktop.empty()&&g.common.empty()))return false;
    if(!CreateDirectoryW(g.data.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    const auto primary=g.data+L"\\hidden-items.dat",backup=g.data+L"\\hidden-items.backup.dat";
    if(!Read(primary)&&!Read(backup)&&(Exists(primary)||Exists(backup)))return false;
    if(!claim){g.ready=true;return true;}
    g.owner=GetCurrentProcessId();FILETIME exit{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&g.born,&exit,&kernel,&user))return false;
    g.ready=Persist();return g.ready;
}
bool StartWatcher(const std::wstring& extra=L"") {
    const auto eventName=L"Local\\MintDesk.VisibilityWatch."+std::to_wstring(GetCurrentProcessId());
    File ready(CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()));if(!ready.value)return false;
    const auto executable=ExecutablePath();std::wstring command=L"\""+executable+L"\" /visibility-watch "+std::to_wstring(GetCurrentProcessId())+extra;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return false;
    CloseHandle(process.hThread);File child(process.hProcess);const HANDLE waits[]={ready.value,child.value};
    return WaitForMultipleObjects(2,waits,FALSE,10000)==WAIT_OBJECT_0;
}
bool RestoreAll(){if(!g.ready)return false;g.permission=false;bool ok=true;for(auto& e:g.entries)if(!Restore(e))ok=false;return ok;}
} // namespace

bool InitializeHiddenItems(){Lock guard;Gate gate;try{return gate.held&&InitializeAt(DataDirectory(),Known(FOLDERID_Desktop),Known(FOLDERID_PublicDesktop),false)&&StartWatcher();}catch(...){return false;}}
DesktopManageResult HideDesktopItem(const std::wstring& reference) {
    Lock guard;Gate gate;
    try {
        const auto path=Normalize(reference);
        if(!Direct(path)||Equal(path,Normalize(ExecutablePath())))return DesktopManageResult::Reference;
        if(!gate.held||!g.ready)return DesktopManageResult::Failed;
        auto found=std::find_if(g.entries.begin(),g.entries.end(),[&](const Entry& e){return Equal(e.path,path);});
        if(found!=g.entries.end()&&found->active) {
            const DWORD error=Change(*found,kVisibilityBits);
            return error==ERROR_ACCESS_DENIED?DesktopManageResult::RequiresPermission:error?DesktopManageResult::Failed:DesktopManageResult::Managed;
        }
        File probe(OpenVisibility(path,false));FileIdentity identity;DWORD attributes=0;
        if(probe.value==INVALID_HANDLE_VALUE||!VisibilityInfo(probe.value,identity,attributes))return DesktopManageResult::Failed;
        if(g.entries.size()>=20000&&found==g.entries.end())return DesktopManageResult::Failed;
        if(found==g.entries.end()){g.entries.emplace_back();found=g.entries.end()-1;}
        found->path=path;found->id=identity;found->original=attributes&kVisibilityBits;found->active=true;
        // Both journals must contain the original bits before any attribute is
        // changed. Failed persistence never permits acquisition.
        if(!Persist()){found->active=false;return DesktopManageResult::Failed;}
        const DWORD error=Change(*found,kVisibilityBits);
        return error==ERROR_ACCESS_DENIED?DesktopManageResult::RequiresPermission:error?DesktopManageResult::Failed:DesktopManageResult::Managed;
    }catch(...){return DesktopManageResult::Failed;}
}
bool IsHiddenDesktopItem(const std::wstring& path){Lock guard;const auto p=Normalize(path);for(const auto& e:g.entries)if(e.active&&Equal(e.path,p)){File probe(OpenVisibility(p,false));FileIdentity id;DWORD attributes=0;return probe.value!=INVALID_HANDLE_VALUE&&VisibilityInfo(probe.value,id,attributes)&&id==e.id&&(attributes&kVisibilityBits)==kVisibilityBits;}return false;}
bool ReleaseHiddenDesktopItem(const std::wstring& path){Lock guard;Gate gate;try{g.permission=false;if(!g.ready||!gate.held)return false;const auto p=Normalize(path);for(auto& e:g.entries)if(Equal(e.path,p))return Restore(e);return true;}catch(...){return false;}}
bool RestoreHiddenItems(){Lock guard;Gate gate;try{return gate.held&&RestoreAll();}catch(...){return false;}}
bool HiddenItemsNeedPermission(){Lock guard;return g.permission;}
void ClearHiddenItems(){Lock guard;g={};}
int Watch(DWORD parentPid,const std::wstring& data,const std::wstring& desktop,const std::wstring& common,bool injected) {
    File parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));
    if(parent.value==INVALID_HANDLE_VALUE||!parent.value||parentPid==GetCurrentProcessId())return 1;
    std::wstring image(32768,L'\0');DWORD count=static_cast<DWORD>(image.size());FILETIME born{},exit{},kernel{},user{};
    if(!QueryFullProcessImageNameW(parent.value,0,&image[0],&count)||!GetProcessTimes(parent.value,&born,&exit,&kernel,&user))return 1;
    image.resize(count);if(!Equal(Normalize(image),Normalize(ExecutablePath())))return 1;
    File ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,(L"Local\\MintDesk.VisibilityWatch."+std::to_wstring(parentPid)).c_str()));
    if(!ready.value||!SetEvent(ready.value))return 1;
    if(WaitForSingleObject(parent.value,INFINITE)!=WAIT_OBJECT_0)return 1;
    Lock guard;Gate gate;
    try {
        if(!gate.held||!InitializeAt(data,desktop,common,injected,false))return 2;
        // A newer UI claims the journal before it starts hiding. A delayed old
        // watcher may restore only its own parent's generation.
        if(g.owner!=parentPid||CompareFileTime(&g.born,&born))return 0;
        for(int attempt=0;attempt<20;++attempt) {
            if(RestoreAll())return 0;
            if(!g.permission)return 2;
            Sleep(100); // Allow the elevated file helper to release its leases.
        }
        return 2;
    }catch(...){return 2;}
}
int RunVisibilityWatcher(DWORD parentPid){return Watch(parentPid,DataDirectory(),Known(FOLDERID_Desktop),Known(FOLDERID_PublicDesktop),false);}
namespace testing {
bool InitializeHiddenItemsAt(const std::wstring& data,const std::wstring& desktop,const std::wstring& common){Lock guard;Gate gate;return gate.held&&InitializeAt(data,desktop,common,true);}
bool RecoverHiddenItemsAt(const std::wstring& data,const std::wstring& desktop,const std::wstring& common,DWORD owner){Lock guard;Gate gate;if(!gate.held||!InitializeAt(data,desktop,common,true,false))return false;return g.owner!=owner||RestoreAll();}
bool StartHiddenItemsWatcherAt(){Lock guard;return g.injected&&StartWatcher(L" \""+g.data+L"\" \""+g.desktop+L"\" \""+g.common+L"\"");}
int RunVisibilityWatcherAt(DWORD pid,const std::wstring& data,const std::wstring& desktop,const std::wstring& common){return Watch(pid,data,desktop,common,true);}
}
} // namespace desk
