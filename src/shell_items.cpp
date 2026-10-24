#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "shell_items.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <commoncontrols.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace desk {
namespace {

// Older MinGW uuid import libraries omit IID_IImageList.
constexpr GUID kImageListInterfaceId = {
    0x46eb5926, 0x582e, 0x4017, {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}
};

class ComApartment {
public:
    ComApartment()
        : result_(CoInitializeEx(nullptr,
              COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    bool usable() const {
        // An existing apartment belongs to the caller; do not uninitialize it.
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
private:
    HRESULT result_;
};

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (ptr_) ptr_->Release(); }
    T** put() { return &ptr_; }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
private:
    T* ptr_ = nullptr;
};

class OwnedIcon {
public:
    explicit OwnedIcon(HICON value = nullptr) : value_(value) {}
    ~OwnedIcon() { if (value_) DestroyIcon(value_); }
    OwnedIcon(const OwnedIcon&) = delete;
    OwnedIcon& operator=(const OwnedIcon&) = delete;
    HICON get() const { return value_; }
    HICON release() { HICON value = value_; value_ = nullptr; return value; }
    void reset(HICON value) {
        if (value_) DestroyIcon(value_);
        value_ = value;
    }
private:
    HICON value_;
};

bool IsSeparator(wchar_t c) {
    return c == L'\\' || c == L'/';
}

bool HasSuffix(const std::wstring& value, const wchar_t* suffix) {
    const std::size_t length = wcslen(suffix);
    if (value.size() < length) return false;
    return _wcsicmp(value.c_str() + value.size() - length, suffix) == 0;
}

std::wstring DisplayLabel(const std::wstring& path) {
    std::size_t end = path.size();
    while (end > 3 && IsSeparator(path[end - 1])) --end;
    const std::size_t slash = path.find_last_of(L"\\/", end - 1);
    std::wstring label = path.substr(slash == std::wstring::npos ? 0 : slash + 1,
                                     end - (slash == std::wstring::npos ? 0 : slash + 1));
    if (label.empty()) label = path.substr(0, end);
    if (HasSuffix(label, L".lnk") || HasSuffix(label, L".url")) {
        label.resize(label.size() - 4);
    }
    return label;
}

HICON OwnedSizedIcon(HICON original, int pixels) {
    if (!original) return nullptr;
    return static_cast<HICON>(CopyImage(original, IMAGE_ICON, pixels, pixels, 0));
}

bool Execute(HWND owner, const std::wstring& path, const wchar_t* verb,
             ULONG extraFlags = 0) {
    if (!IsSupportedPath(path)) return false;
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    // Calls run on a dedicated worker without a message pump. Complete the
    // shell/DDE handoff before returning instead of leaving STA work queued.
    execute.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | extraFlags;
    execute.hwnd = owner;
    execute.lpVerb = verb;
    execute.lpFile = path.c_str();
    execute.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&execute) != FALSE;
}

} // namespace

bool IsSupportedPath(const std::wstring& path) {
    // Reject commands, URLs, control characters and malformed shell arguments.
    // Actual file existence is deliberately left to the requested shell action.
    if (path.empty() || path.size() > 32767) return false;
    for (wchar_t c : path) {
        if (c < 32 || c == L'"' || c == L'<' || c == L'>' ||
            c == L'|' || c == L'*' || c == L'?') return false;
    }

    const bool drivePath = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && IsSeparator(path[2]);
    if (drivePath) return path.find(L':', 2) == std::wstring::npos;

    // UNC paths need both a server and a share. Device namespaces are excluded.
    if (path.size() < 5 || !IsSeparator(path[0]) || !IsSeparator(path[1]) ||
        IsSeparator(path[2]) || path[2] == L'.' || path[2] == L'?' ||
        path.find(L':') != std::wstring::npos) return false;
    const std::size_t serverEnd = path.find_first_of(L"\\/", 2);
    return serverEnd != std::wstring::npos && serverEnd + 1 < path.size() &&
        !IsSeparator(path[serverEnd + 1]);
}

ShellVisual LoadShellVisual(const std::wstring& path, int pixelSize) {
    ShellVisual visual;
    visual.label = DisplayLabel(path);
    if (!IsSupportedPath(path)) return visual;

    const int pixels = (std::max)(16, (std::min)(pixelSize, 256));
    const bool networkPath = IsSeparator(path[0]) && IsSeparator(path[1]);
    ComApartment apartment;
    SHFILEINFOW info{};
    const UINT iconSize = pixels <= GetSystemMetrics(SM_CXSMICON)
        ? SHGFI_SMALLICON : SHGFI_LARGEICON;
    // Merely displaying a network reference must never connect to its server.
    // Keep its lexical label and use a locally resolved file-type icon below.
    const DWORD_PTR result = networkPath ? 0 : SHGetFileInfoW(path.c_str(), 0,
        &info, sizeof(info), SHGFI_ICON | SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | iconSize);
    OwnedIcon original(info.hIcon), selected;
    if (result && info.szDisplayName[0]) {
        visual.label = info.szDisplayName;
        if (HasSuffix(visual.label, L".lnk") || HasSuffix(visual.label, L".url")) {
            visual.label.resize(visual.label.size() - 4);
        }
    }
    if (result && pixels > GetSystemMetrics(SM_CXICON) && apartment.usable()) {
        ComPtr<IImageList> imageList;
        const int listSize = pixels > 96 ? SHIL_JUMBO : SHIL_EXTRALARGE;
        if (SUCCEEDED(SHGetImageList(listSize, kImageListInterfaceId,
                reinterpret_cast<void**>(imageList.put())))) {
            HICON rawLarger = nullptr;
            const HRESULT extracted = imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &rawLarger);
            OwnedIcon larger(rawLarger);
            if (SUCCEEDED(extracted) && larger.get()) {
                selected.reset(OwnedSizedIcon(larger.get(), pixels));
                if (!selected.get()) selected.reset(larger.release());
            }
        }
    }
    if (original.get() && !selected.get()) {
        selected.reset(OwnedSizedIcon(original.get(), pixels));
        if (!selected.get()) selected.reset(original.release());
    }
    if (!selected.get()) {
        // A removed or unavailable item still gets a stable, locally resolved icon.
        SHFILEINFOW fallback{};
        const wchar_t* genericPath = path.c_str();
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        if (networkPath) {
            // Do not even pass a UNC prefix to the shell, despite the attribute
            // flag already prohibiting access to the named file.
            genericPath += path.find_last_of(L"\\/") + 1;
            if (!*genericPath) {
                genericPath = L"folder";
                attributes = FILE_ATTRIBUTE_DIRECTORY;
            }
        }
        const DWORD_PTR fallbackResult = SHGetFileInfoW(genericPath, attributes,
            &fallback, sizeof(fallback), SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | iconSize);
        OwnedIcon generic(fallback.hIcon);
        if (fallbackResult && generic.get()) {
            selected.reset(OwnedSizedIcon(generic.get(), pixels));
            if (!selected.get()) selected.reset(generic.release());
        }
        if (!selected.get()) {
            selected.reset(OwnedSizedIcon(LoadIconW(nullptr, MAKEINTRESOURCEW(32512)), pixels));
        }
    }
    visual.icon = selected.release();
    return visual;
}

bool OpenItem(HWND owner, const std::wstring& path) {
    return Execute(owner, path, L"open");
}

bool ShowItemProperties(HWND owner, const std::wstring& path) {
    return Execute(owner, path, L"properties", SEE_MASK_INVOKEIDLIST);
}

bool RevealItem(HWND, const std::wstring& path) {
    if (!IsSupportedPath(path)) return false;
    ComApartment apartment;
    if (!apartment.usable()) return false;
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)) || !item) {
        return false;
    }
    PIDLIST_ABSOLUTE parent = ILCloneFull(item);
    HRESULT result = E_OUTOFMEMORY;
    if (parent) {
        PCUITEMID_CHILD child = ILFindLastID(item);
        if (ILRemoveLastID(parent)) {
            result = SHOpenFolderAndSelectItems(parent, 1, &child, 0);
        } else {
            result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        }
        CoTaskMemFree(parent);
    }
    CoTaskMemFree(item);
    return SUCCEEDED(result);
}

std::vector<std::wstring> PickItems(HWND owner, bool folders) {
    std::vector<std::wstring> paths;
    ComApartment apartment;
    if (!apartment.usable()) return paths;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                               IID_IFileOpenDialog,
                               reinterpret_cast<void**>(dialog.put())))) return paths;
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->GetOptions(&options))) return paths;
    options |= FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR |
        FOS_DONTADDTORECENT | FOS_PATHMUSTEXIST;
    options |= folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    if (FAILED(dialog->SetOptions(options)) || FAILED(dialog->Show(owner))) return paths;
    ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(items.put()))) return paths;
    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) return paths;
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(index, item.put()))) continue;
        PWSTR rawPath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
            std::wstring path(rawPath);
            CoTaskMemFree(rawPath);
            if (IsSupportedPath(path)) paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::vector<std::wstring> PathsFromDrop(HDROP drop) {
    std::vector<std::wstring> paths;
    if (!drop) return paths;
    const UINT count = DragQueryFileW(drop, (std::numeric_limits<UINT>::max)(), nullptr, 0);
    paths.reserve((std::min)(count, 4096u));
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (!length || length > 32767) continue;
        std::wstring path(length + 1, L'\0');
        const UINT copied = DragQueryFileW(drop, index, &path[0], length + 1);
        if (copied != length) continue;
        path.resize(length);
        if (IsSupportedPath(path)) paths.push_back(std::move(path));
    }
    // The owner of HDROP is responsible for DragFinish (or ReleaseStgMedium).
    return paths;
}

} // namespace desk
