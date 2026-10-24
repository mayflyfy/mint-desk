#include "file_helper.h"
#include "state.h"
#include <shlobj.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <memory>

namespace desk {
namespace {
constexpr DWORD kMagic=0x36464344, kVersion=1, kMaxMessage=132000;
enum Operation : DWORD { Hello=0, Probe=1, Move=2, Visibility=3 };
struct Header { DWORD magic, version, operation, source, target; };
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
struct Roots { std::wstring desktop, common, managed; };
HANDLE clientPipe=INVALID_HANDLE_VALUE, helperProcess=nullptr;
bool consentDeclined=false;
bool operationUncertain=false;
HANDLE moveMutex=nullptr;
PSECURITY_DESCRIPTOR PrivateSecurity(const wchar_t* suffix=L"") {
    Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return nullptr;
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> user(size);
    if(!size||!GetTokenInformation(token.value,TokenUser,user.data(),size,&size))return nullptr;
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,&sid))return nullptr;
    const std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+std::wstring(sid)+L")"+suffix;LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return nullptr;
    return descriptor;
}
struct MoveGate {
    bool held=false;
    MoveGate() {
        if(!moveMutex) {
            PSECURITY_DESCRIPTOR descriptor=PrivateSecurity(L"S:(ML;;NW;;;ME)");if(!descriptor)return;
            SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
            moveMutex=CreateMutexW(&attributes,FALSE,L"Local\\DeskCards.FileMoves.1");LocalFree(descriptor);
        }
        if(!moveMutex)return;
        const DWORD wait=WaitForSingleObject(moveMutex,10000);held=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;
    }
    ~MoveGate(){if(held)ReleaseMutex(moveMutex);}
};

bool Equal(const std::wstring& a,const std::wstring& b) { return !a.empty()&&!b.empty()&&CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL; }
std::wstring Parent(const std::wstring& p) {const auto at=p.find_last_of(L'\\');return at==std::wstring::npos?L"":p.substr(0,at);}
std::wstring Name(const std::wstring& p) {return p.substr(p.find_last_of(L'\\')+1);}
std::wstring Canonical(const std::wstring& p) {
    if(p.size()<3||p.size()>32760||p[1]!=L':'||(p[2]!=L'\\'&&p[2]!=L'/')||p.find(L'\0')!=std::wstring::npos)return {};
    for(size_t at=3;at<p.size();) {
        const auto end=p.find_first_of(L"\\/",at);const auto part=p.substr(at,end-at);
        if(part.empty()||part.back()==L'.'||part.back()==L' ')return {};
        if(end==std::wstring::npos)break;
        at=end+1;
    }
    const DWORD count=GetFullPathNameW(p.c_str(),0,nullptr,nullptr);if(!count||count>32768)return {};
    std::wstring out(count,L'\0');const DWORD used=GetFullPathNameW(p.c_str(),count,&out[0],nullptr);
    if(!used||used>=count)return {};
    out.resize(used);std::replace(out.begin(),out.end(),L'/',L'\\');
    while(out.size()>3&&out.back()==L'\\')out.pop_back();
    // Reject alternate streams and Win32 aliases, including trailing dot/space.
    if(out.find(L':',2)!=std::wstring::npos)return {};
    for(size_t at=3;at<out.size();) {const auto end=out.find(L'\\',at);const auto part=out.substr(at,end-at);
        if(part.empty()||part.back()==L'.'||part.back()==L' '||part.find_first_of(L"<>\"|?*")!=std::wstring::npos)return {};
        if(std::any_of(part.begin(),part.end(),[](wchar_t c){return c<L' ';}))return {};
        auto base=part.substr(0,part.find(L'.'));for(auto& c:base)c=static_cast<wchar_t>(towupper(c));
        if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||
            (base.size()==4&&(base.substr(0,3)==L"COM"||base.substr(0,3)==L"LPT")&&base[3]>=L'1'&&base[3]<=L'9'))return {};
        if(end==std::wstring::npos)break;
        at=end+1;}
    return out;
}
std::wstring Known(REFKNOWNFOLDERID id) {PWSTR p=nullptr;if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&p)))return {};std::wstring out=p;CoTaskMemFree(p);return Canonical(out);}
Roots RealRoots() {return {Known(FOLDERID_Desktop),Known(FOLDERID_PublicDesktop),Canonical(DataDirectory()+L"\\Managed")};}
bool Desktop(const Roots& r,const std::wstring& p) {return Equal(p,r.desktop)||Equal(p,r.common);}
bool Slot(const Roots& r,const std::wstring& file) {
    const auto dir=Parent(file);if(!Equal(Parent(dir),r.managed))return false;
    const auto slot=Name(dir);const auto dash=slot.find(L'-');
    return dash!=std::wstring::npos&&dash>0&&dash+1<slot.size()&&slot.find(L'-',dash+1)==std::wstring::npos&&slot.size()<64&&
        std::all_of(slot.begin(),slot.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||c==L'-';});
}
bool Allowed(const Roots& r,const std::wstring& from,const std::wstring& to) {
    if(from.empty()||to.empty()||Equal(from,to))return false;
    return (Desktop(r,Parent(from))&&Slot(r,to)&&Name(from)==Name(to))||
        (Slot(r,from)&&Desktop(r,Parent(to)));
}
struct PinnedDirectories {
    std::vector<HANDLE> handles;
    ~PinnedDirectories(){for(HANDLE h:handles)CloseHandle(h);}
    bool Add(const std::wstring& path) {
        for(size_t end=3;;) {
            end=path.find(L'\\',end);const auto part=path.substr(0,end);
            Handle h(CreateFileW(part.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if(h.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h.value,&info)||
                !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
            handles.push_back(h.value);h.value=nullptr;
            if(end==std::wstring::npos)break;
            ++end;
        }
        return true;
    }
};
bool SameVolume(const std::wstring& a,const std::wstring& b) {
    wchar_t av[32768]{},bv[32768]{},aid[64]{},bid[64]{};
    return GetVolumePathNameW(a.c_str(),av,32768)&&GetVolumePathNameW(b.c_str(),bv,32768)&&
        GetVolumeNameForVolumeMountPointW(av,aid,64)&&GetVolumeNameForVolumeMountPointW(bv,bid,64)&&!_wcsicmp(aid,bid);
}
DWORD ProbeDirectory(const Roots& r,const std::wstring& path) {
    PinnedDirectories pinned;
    if(!Desktop(r,path)||!pinned.Add(path))return ERROR_ACCESS_DENIED;
    const auto probe=path+L"\\.MintDesk-permission-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    {Handle f(CreateFileW(probe.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));if(f.value==INVALID_HANDLE_VALUE)return GetLastError();}
    const auto dir=probe+L"-dir";if(!CreateDirectoryW(dir.c_str(),nullptr))return GetLastError();
    return RemoveDirectoryW(dir.c_str())?ERROR_SUCCESS:GetLastError();
}
DWORD ExecuteMove(const Roots& r,const std::wstring& from,const std::wstring& to,HANDLE parent=nullptr) {
    MoveGate gate;
    if(!gate.held)return ERROR_TIMEOUT;
    if(parent&&WaitForSingleObject(parent,0)!=WAIT_TIMEOUT)return ERROR_CANCELLED;
    PinnedDirectories pinned;
    if(!Allowed(r,from,to)||!pinned.Add(Parent(from))||!pinned.Add(Parent(to))||!SameVolume(from,Parent(to)))return ERROR_ACCESS_DENIED;
    const DWORD attributes=GetFileAttributesW(from.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)return GetLastError();
    if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)return ERROR_ACCESS_DENIED;
    // No replacement, copying, ACL changes, deletion, or process execution.
    if(!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH))return GetLastError();
    const bool directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    SHChangeNotify(directory?SHCNE_RMDIR:SHCNE_DELETE,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,from.c_str(),nullptr);
    SHChangeNotify(directory?SHCNE_MKDIR:SHCNE_CREATE,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,to.c_str(),nullptr);
    return ERROR_SUCCESS;
}
struct VisibilityLease {
    Handle file;
    FileIdentity id;
    DWORD original=0;
    std::wstring path;
    explicit VisibilityLease(HANDLE h):file(h){}
    ~VisibilityLease(){
        if(file.value&&file.value!=INVALID_HANDLE_VALUE&&ChangeVisibility(file.value,id,original)==ERROR_SUCCESS){
            const auto current=VisibilityPath(file.value,path);
            SHChangeNotify(SHCNE_ATTRIBUTES,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,current.c_str(),nullptr);
            SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW|SHCNF_FLUSHNOWAIT,current.c_str(),nullptr);
        }
    }
};
DWORD ExecuteVisibility(const Roots& r,const std::wstring& path,const FileIdentity& expected,DWORD bits,
    std::vector<std::unique_ptr<VisibilityLease>>* leases=nullptr,HANDLE parent=nullptr) {
    MoveGate gate;PinnedDirectories pinned;
    if(!gate.held)return ERROR_TIMEOUT;
    if(parent&&WaitForSingleObject(parent,0)!=WAIT_TIMEOUT)return ERROR_CANCELLED;
    if(path.empty()||!Desktop(r,Parent(path))||(bits&~kVisibilityBits)||!pinned.Add(Parent(path)))return ERROR_ACCESS_DENIED;
    if(leases)for(auto it=leases->begin();it!=leases->end();++it)if((*it)->id==expected){
        const DWORD error=ChangeVisibility((*it)->file.value,expected,bits);
        if(!error&&bits==(*it)->original)leases->erase(it);
        return error;
    }
    Handle file(OpenVisibility(path,true));if(file.value==INVALID_HANDLE_VALUE)return GetLastError();
    FileIdentity actual;DWORD attributes=0;
    if(!VisibilityInfo(file.value,actual,attributes))return GetLastError();
    if(!(actual==expected))return ERROR_FILE_INVALID;
    // Allocate the recovery lease before changing any bits. It owns a handle
    // to the exact file, including if the user later renames it.
    VisibilityLease* lease=nullptr;
    if(leases&&bits==kVisibilityBits&&(attributes&kVisibilityBits)!=bits){
        auto pending=std::make_unique<VisibilityLease>(file.value);
        pending->id=actual;pending->path=path;pending->original=attributes&kVisibilityBits;
        file.value=nullptr;leases->push_back(std::move(pending));lease=leases->back().get();
    }
    return ChangeVisibility(lease?lease->file.value:file.value,expected,bits);
}
std::vector<BYTE> UserToken(HANDLE process) {
    Handle token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.value))return {};
    DWORD n=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&n);std::vector<BYTE> b(n);
    if(!n||!GetTokenInformation(token.value,TokenUser,b.data(),n,&n))return {};
    return b;
}
std::wstring Image(HANDLE process) {std::wstring p(32768,L'\0');DWORD n=static_cast<DWORD>(p.size());if(!QueryFullProcessImageNameW(process,0,&p[0],&n))return {};p.resize(n);return Canonical(p);}
bool TrustedParent(HANDLE parent,DWORD pid) {
    if(!Equal(Image(parent),Canonical(ExecutablePath()))||WaitForSingleObject(parent,0)!=WAIT_TIMEOUT)return false;
    DWORD ours=0,theirs=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&ours)||!ProcessIdToSessionId(pid,&theirs)||ours!=theirs)return false;
    const auto a=UserToken(GetCurrentProcess()),b=UserToken(parent);
    return !a.empty()&&!b.empty()&&EqualSid(reinterpret_cast<const TOKEN_USER*>(a.data())->User.Sid,reinterpret_cast<const TOKEN_USER*>(b.data())->User.Sid);
}
bool Elevated() {Handle t;TOKEN_ELEVATION e{};DWORD n=0;return OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&t.value)&&GetTokenInformation(t.value,TokenElevation,&e,sizeof(e),&n)&&e.TokenIsElevated;}
std::wstring PipeName(DWORD pid,const std::wstring& nonce) {return L"\\\\.\\pipe\\MintDesk.Files."+std::to_wstring(pid)+L"."+nonce;}
bool ValidNonce(const std::wstring& n) {return n.size()==32&&std::all_of(n.begin(),n.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});}
std::wstring NewNonce() {GUID g{};if(FAILED(CoCreateGuid(&g)))return {};const auto* p=reinterpret_cast<const BYTE*>(&g);std::wstring out;for(size_t i=0;i<sizeof(g);++i){out.push_back(L"0123456789abcdef"[p[i]>>4]);out.push_back(L"0123456789abcdef"[p[i]&15]);}return out;}
DWORD Transfer(HANDLE pipe,bool write,void* bytes,DWORD size,DWORD& done,HANDLE parent=nullptr) {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return GetLastError();OVERLAPPED o{};o.hEvent=event.value;
    const BOOL okay=write?WriteFile(pipe,bytes,size,&done,&o):ReadFile(pipe,bytes,size,&done,&o);
    if(okay)return ERROR_SUCCESS;
    const DWORD error=GetLastError();if(error!=ERROR_IO_PENDING)return error;
    const HANDLE waits[]={event.value,parent};const DWORD wait=WaitForMultipleObjects(parent?2:1,waits,FALSE,parent?INFINITE:10000);
    if(wait!=WAIT_OBJECT_0){CancelIoEx(pipe,&o);GetOverlappedResult(pipe,&o,&done,TRUE);return wait==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_BROKEN_PIPE;}
    return GetOverlappedResult(pipe,&o,&done,FALSE)?ERROR_SUCCESS:GetLastError();
}
DWORD Request(DWORD op,const std::wstring& a={},const std::wstring& b={}) {
    if(!FileHelperRunning()||a.size()>32760||b.size()>32760)return ERROR_PRIVILEGE_NOT_HELD;
    Header h{kMagic,kVersion,op,static_cast<DWORD>(a.size()),static_cast<DWORD>(b.size())};
    std::vector<BYTE> bytes(sizeof(h)+(a.size()+b.size())*sizeof(wchar_t));std::memcpy(bytes.data(),&h,sizeof(h));
    if(!a.empty())std::memcpy(bytes.data()+sizeof(h),a.data(),a.size()*sizeof(wchar_t));
    if(!b.empty())std::memcpy(bytes.data()+sizeof(h)+a.size()*sizeof(wchar_t),b.data(),b.size()*sizeof(wchar_t));
    DWORD done=0;DWORD error=Transfer(clientPipe,true,bytes.data(),static_cast<DWORD>(bytes.size()),done);
    if(!error&&done!=bytes.size())error=ERROR_INVALID_DATA;
    DWORD response=ERROR_INVALID_DATA;
    if(!error)error=Transfer(clientPipe,false,&response,sizeof(response),done);
    if(!error&&done!=sizeof(response))error=ERROR_INVALID_DATA;
    if(!error)return response;
    // The child may have committed a rename before its reply was lost. Keep
    // the durable intent and stop further operations until the child exits.
    if(clientPipe!=INVALID_HANDLE_VALUE){CloseHandle(clientPipe);clientPipe=INVALID_HANDLE_VALUE;}
    operationUncertain=op==Move||op==Visibility;
    return operationUncertain?ERROR_IO_INCOMPLETE:error;
}
}

bool FileHelperRunning() {return clientPipe!=INVALID_HANDLE_VALUE&&helperProcess&&WaitForSingleObject(helperProcess,0)==WAIT_TIMEOUT;}
bool FileHelperOperationPending() {
    if(!operationUncertain)return false;
    if(helperProcess&&WaitForSingleObject(helperProcess,0)==WAIT_TIMEOUT)return true;
    operationUncertain=false;return false;
}
bool WaitForFileMovesIdle(){MoveGate gate;return gate.held;}
void StopFileHelper() {
    if(clientPipe!=INVALID_HANDLE_VALUE){CloseHandle(clientPipe);clientPipe=INVALID_HANDLE_VALUE;}
    if(helperProcess){CloseHandle(helperProcess);helperProcess=nullptr;}
}
bool EnsureFileHelper(HWND owner,bool retryConsent) {
    static bool launching=false;
    if(launching)return false;
    if(FileHelperOperationPending())return false;
    if(FileHelperRunning())return true;
    if(consentDeclined&&!retryConsent)return false;
    if(!WaitForFileMovesIdle())return false;
    struct LaunchScope {bool& flag;explicit LaunchScope(bool& f):flag(f){flag=true;}~LaunchScope(){flag=false;}} scope(launching);
    StopFileHelper();
    const auto user=UserToken(GetCurrentProcess());if(user.empty())return false;
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,&sid))return false;
    const std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return false;
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const auto nonce=NewNonce();if(!ValidNonce(nonce)){LocalFree(descriptor);return false;}
    clientPipe=CreateNamedPipeW(PipeName(GetCurrentProcessId(),nonce).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,kMaxMessage,kMaxMessage,0,&security);LocalFree(descriptor);
    if(clientPipe==INVALID_HANDLE_VALUE)return false;
    const auto executable=ExecutablePath();const auto arguments=L"/file-helper "+std::to_wstring(GetCurrentProcessId())+L" "+nonce;
    SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
    launch.hwnd=owner;launch.lpVerb=L"runas";launch.lpFile=executable.c_str();launch.lpParameters=arguments.c_str();launch.nShow=SW_HIDE;
    if(!ShellExecuteExW(&launch)){consentDeclined=GetLastError()==ERROR_CANCELLED;StopFileHelper();return false;}
    helperProcess=launch.hProcess;
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED o{};o.hEvent=event.value;
    BOOL connected=ConnectNamedPipe(clientPipe,&o);const DWORD connectError=connected?ERROR_SUCCESS:GetLastError();
    if(!connected&&connectError==ERROR_IO_PENDING){const HANDLE waits[]={event.value,helperProcess};
        if(WaitForMultipleObjects(2,waits,FALSE,15000)==WAIT_OBJECT_0){DWORD transferred=0;connected=GetOverlappedResult(clientPipe,&o,&transferred,FALSE);}
        else {CancelIoEx(clientPipe,&o);DWORD ignored=0;GetOverlappedResult(clientPipe,&o,&ignored,TRUE);}}
    else if(connectError==ERROR_PIPE_CONNECTED)connected=TRUE;
    ULONG pid=0;
    if(!connected||!GetNamedPipeClientProcessId(clientPipe,&pid)||pid!=GetProcessId(helperProcess)){StopFileHelper();return false;}
    if(Request(Hello)!=ERROR_SUCCESS){StopFileHelper();return false;}
    consentDeclined=false;return true;
}
DWORD CheckPrivilegedDesktopRestore(const std::wstring& path) {return Request(Probe,path);}
DWORD PrivilegedDesktopMove(const std::wstring& source,const std::wstring& target) {return Request(Move,source,target);}
DWORD PrivilegedDesktopVisibility(const std::wstring& path,const FileIdentity& expected,DWORD bits) {
    if(bits&~kVisibilityBits)return ERROR_INVALID_PARAMETER;
    return Request(Visibility,path,std::to_wstring(expected.volume)+L" "+std::to_wstring(expected.high)+L" "+
        std::to_wstring(expected.low)+L" "+std::to_wstring(bits));
}

int RunFileHelper(DWORD parentPid,const std::wstring& nonce) {
    if(!parentPid||!ValidNonce(nonce)||!Elevated())return 1;
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));
    if(!parent.value||!TrustedParent(parent.value,parentPid))return 1;
    const auto path=PipeName(parentPid,nonce);
    Handle pipe(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    if(pipe.value==INVALID_HANDLE_VALUE)return 1;
    ULONG server=0;if(!GetNamedPipeServerProcessId(pipe.value,&server)||server!=parentPid)return 1;
    DWORD mode=PIPE_READMODE_MESSAGE;if(!SetNamedPipeHandleState(pipe.value,&mode,nullptr,nullptr))return 1;
    const Roots roots=RealRoots();std::vector<BYTE> data(kMaxMessage);
    std::vector<std::unique_ptr<VisibilityLease>> leases;
    for(;;){DWORD size=0;const DWORD read=Transfer(pipe.value,false,data.data(),static_cast<DWORD>(data.size()),size,parent.value);if(read)break;
        DWORD result=ERROR_INVALID_DATA;
        if(size>=sizeof(Header)){Header h{};std::memcpy(&h,data.data(),sizeof(h));
            if(h.magic==kMagic&&h.version==kVersion&&h.source<=32760&&h.target<=32760&&
                sizeof(h)+(static_cast<size_t>(h.source)+h.target)*sizeof(wchar_t)==size){
                const auto* text=reinterpret_cast<const wchar_t*>(data.data()+sizeof(h));
                const std::wstring a(text,h.source),b(text+h.source,h.target);
                const auto from=Canonical(a),to=Canonical(b);
                if(h.operation==Hello&&a.empty()&&b.empty())result=ERROR_SUCCESS;
                else if(h.operation==Probe&&b.empty()&&!from.empty())result=ProbeDirectory(roots,from);
                else if(h.operation==Move)result=ExecuteMove(roots,from,to,parent.value);
                else if(h.operation==Visibility) {
                    DWORD values[4]{};const wchar_t* cursor=b.c_str();bool valid=true;
                    for(int i=0;i<4;++i){wchar_t* end=nullptr;if(!*cursor||*cursor<L'0'||*cursor>L'9'){valid=false;break;}
                        const unsigned long long value=wcstoull(cursor,&end,10);
                        if(end==cursor||value>MAXDWORD||(i<3?*end!=L' ':*end!=L'\0')){valid=false;break;}
                        values[i]=static_cast<DWORD>(value);cursor=i<3?end+1:end;}
                    if(valid)result=ExecuteVisibility(roots,from,{values[0],values[1],values[2]},values[3],&leases,parent.value);
                }
            }}
        DWORD written=0;if(Transfer(pipe.value,true,&result,sizeof(result),written,parent.value))break;
    }
    return 0;
}
namespace testing {
bool IsAllowedDesktopMove(const std::wstring& desktop,const std::wstring& common,const std::wstring& managed,
    const std::wstring& source,const std::wstring& target) {
    return Allowed({Canonical(desktop),Canonical(common),Canonical(managed)},Canonical(source),Canonical(target));
}
DWORD MoveDesktopItemAt(const std::wstring& desktop,const std::wstring& common,const std::wstring& managed,
    const std::wstring& source,const std::wstring& target) {
    return ExecuteMove({Canonical(desktop),Canonical(common),Canonical(managed)},Canonical(source),Canonical(target));
}
DWORD DesktopVisibilityAt(const std::wstring& desktop,const std::wstring& common,const std::wstring& path,const FileIdentity& expected,DWORD bits) {
    return ExecuteVisibility({Canonical(desktop),Canonical(common),{}},Canonical(path),expected,bits);
}
}
}
