#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace desk {
constexpr DWORD kVisibilityBits=FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM;
struct FileIdentity {
    DWORD volume=0,high=0,low=0;
    bool operator==(const FileIdentity& other)const{return volume==other.volume&&high==other.high&&low==other.low;}
};
inline bool VisibilityInfo(HANDLE file,FileIdentity& id,DWORD& attributes) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info))return false;
    id={info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};attributes=info.dwFileAttributes;
    return true;
}
inline HANDLE OpenVisibility(const std::wstring& path,bool write) {
    return CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|(write?FILE_WRITE_ATTRIBUTES:0),
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
}
inline DWORD ChangeVisibility(HANDLE file,const FileIdentity& expected,DWORD bits) {
    if(bits&~kVisibilityBits)return ERROR_INVALID_PARAMETER;
    FileIdentity id;DWORD attributes=0;
    if(!VisibilityInfo(file,id,attributes))return GetLastError();
    if(!(id==expected))return ERROR_FILE_INVALID;
    FILE_BASIC_INFO basic{};
    basic.FileAttributes=(attributes&~(kVisibilityBits|FILE_ATTRIBUTE_NORMAL))|bits;
    if(!basic.FileAttributes)basic.FileAttributes=FILE_ATTRIBUTE_NORMAL;
    // Zero timestamps mean preserve the current values. Only two attribute
    // bits change, on this exact file handle; directory contents are untouched.
    return SetFileInformationByHandle(file,FileBasicInfo,&basic,sizeof(basic))?ERROR_SUCCESS:GetLastError();
}
inline std::wstring VisibilityPath(HANDLE file,const std::wstring& fallback) {
    std::wstring path(32768,L'\0');
    const DWORD size=GetFinalPathNameByHandleW(file,&path[0],static_cast<DWORD>(path.size()),FILE_NAME_NORMALIZED);
    if(!size||size>=path.size())return fallback;
    path.resize(size);
    if(path.compare(0,8,L"\\\\?\\UNC\\")==0)return L"\\\\"+path.substr(8);
    if(path.compare(0,4,L"\\\\?\\")==0)return path.substr(4);
    return path;
}
} // namespace desk
