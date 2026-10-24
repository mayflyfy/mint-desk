#include "dragdrop.h"
#include "shell_items.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace desk {
namespace {

constexpr wchar_t kTargetProperty[] = L"MintDesk.FileDrop.Target.4980D6DC";
constexpr SIZE_T kMaximumDropBytes = 16u * 1024u * 1024u;

class MethodReference {
public:
    explicit MethodReference(IUnknown* object) : object_(object) { object_->AddRef(); }
    ~MethodReference() { object_->Release(); }
private:
    IUnknown* object_;
};

DWORD SafeEffect(DWORD allowed, DWORD keys) {
    if ((keys & (MK_CONTROL | MK_SHIFT)) == (MK_CONTROL | MK_SHIFT) &&
        (allowed & DROPEFFECT_LINK)) return DROPEFFECT_LINK;
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return allowed & DROPEFFECT_LINK;
}

FORMATETC FileFormat() {
    return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

CLIPFORMAT PreferredEffectFormat() {
    return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
}

// Bound the shell parser to an actual, terminated, reasonably sized allocation.
// Neither this check nor PathsFromDrop looks up the supplied paths on disk.
bool WellFormedDrop(HGLOBAL memory) {
    const SIZE_T bytes = GlobalSize(memory);
    if (bytes < sizeof(DROPFILES) + 2 || bytes > kMaximumDropBytes) return false;
    const BYTE* raw = static_cast<const BYTE*>(GlobalLock(memory));
    if (!raw) return false;
    DROPFILES header{};
    std::memcpy(&header, raw, sizeof(header));
    bool valid = false;
    if (header.pFiles >= sizeof(DROPFILES) && header.pFiles < bytes) {
        const SIZE_T namesBytes = bytes - header.pFiles;
        if (header.fWide) {
            if (header.pFiles % alignof(wchar_t) == 0 && namesBytes >= 2 * sizeof(wchar_t)) {
                const auto* names = reinterpret_cast<const wchar_t*>(raw + header.pFiles);
                for (SIZE_T i = 1; i < namesBytes / sizeof(wchar_t); ++i) {
                    if (!names[i - 1] && !names[i]) { valid = true; break; }
                }
            }
        } else {
            for (SIZE_T i = 1; i < namesBytes; ++i) {
                if (!raw[header.pFiles + i - 1] && !raw[header.pFiles + i]) {
                    valid = true;
                    break;
                }
            }
        }
    }
    GlobalUnlock(memory);
    return valid && DragQueryFileW(reinterpret_cast<HDROP>(memory),
        (std::numeric_limits<UINT>::max)(), nullptr, 0) <= 4096;
}

std::vector<std::wstring> ReadPaths(IDataObject* data) {
    std::vector<std::wstring> paths;
    if (!data) return paths;
    FORMATETC format = FileFormat();
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) return paths;
    try {
        if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal && WellFormedDrop(medium.hGlobal)) {
            paths = PathsFromDrop(reinterpret_cast<HDROP>(medium.hGlobal));
        }
    } catch (...) {
        ReleaseStgMedium(&medium);
        throw;
    }
    ReleaseStgMedium(&medium);
    return paths;
}

class FileDropTarget final : public IDropTarget {
public:
    FileDropTarget(HWND window,const DropCallbacks& callbacks,IDropTargetHelper* helper=nullptr) : window_(window),callbacks_(callbacks) {
        if(helper){helper_=helper;helper_->AddRef();injectedHelper_=true;}
    }
    ~FileDropTarget(){if(helper_)helper_->Release();}

    void Deactivate() {
        const bool leave=helperActive_;
        helperActive_=false;
        active_ = false;
        callbacks_ = {};
        paths_.clear();
        if(leave&&helper_)helper_->DragLeave();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDropTarget) return E_NOINTERFACE;
        *result = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point,
                                        DWORD* effect) override {
        if (!effect) return E_POINTER;
        MethodReference keepAlive(this);
        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;
        if(helperActive_){helperActive_=false;helper_->DragLeave();}
        paths_.clear();
        if (!active_) return S_OK;
        try { paths_ = ReadPaths(data); }
        catch (...) { return E_OUTOFMEMORY; }
        if (!paths_.empty()) {
            *effect=SafeEffect(allowed,keys);
            POINT at{point.x,point.y};
            if(*effect&&(injectedHelper_||HasDragImage(data))) {
                if(!helper_)CoCreateInstance(CLSID_DragDropHelper,nullptr,CLSCTX_INPROC_SERVER,IID_IDropTargetHelper,reinterpret_cast<void**>(&helper_));
                if(helper_){helperActive_=helper_->DragEnter(window_,data,&at,VisualEffect(*effect,keys))==S_OK&&active_;if(!helperActive_)helper_->DragLeave();}
            }
            Hover(true, {point.x, point.y});
            if(!active_)*effect=DROPEFFECT_NONE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override {
        if (!effect) return E_POINTER;
        MethodReference keepAlive(this);
        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;
        if (active_ && !paths_.empty()) {
            Hover(true, {point.x, point.y});
            if (active_) {
                *effect = SafeEffect(allowed, keys);
                POINT at{point.x,point.y};if(helperActive_)helper_->DragOver(&at,VisualEffect(*effect,keys));
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        MethodReference keepAlive(this);
        const bool leave=helperActive_;
        helperActive_=false;
        if(leave)helper_->DragLeave();
        paths_.clear();
        Hover(false, {});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point,
                                   DWORD* effect) override {
        if (!effect) return E_POINTER;
        MethodReference keepAlive(this);
        const DWORD offered = SafeEffect(*effect, keys);
        const DWORD imageEffect=VisualEffect(offered,keys);
        *effect = DROPEFFECT_NONE;
        // Hide Explorer's drag image before disk work, elevation, or painting.
        // Otherwise the source ghost can linger after the file already moved.
        if(helperActive_)helper_->Show(FALSE);
        // Move out before callbacks: they may revoke this registration, or
        // begin another drag while this OLE method is still on the stack.
        std::vector<std::wstring> paths;
        paths.swap(paths_);
        Hover(false, {point.x, point.y});
        if (active_ && offered && !paths.empty() && callbacks_.drop) {
            const DropCallbacks callbacks = callbacks_;
            try {
                if (callbacks.drop(callbacks.context, paths, {point.x, point.y},keys)) {
                    *effect = offered;
                }
            } catch (...) { /* Never let application exceptions cross COM. */ }
        }
        if(helperActive_) {
            helperActive_=false;
            POINT at{point.x,point.y};
            if(data)helper_->Drop(data,&at,*effect?imageEffect:DROPEFFECT_NONE);
            else helper_->DragLeave();
        }
        return S_OK;
    }
private:
    bool HasDragImage(IDataObject* data) {
        if(!data)return false;
        for(const auto* name:{L"DragImageBits",L"IsShowingLayered"}) {
            FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
            if(data->QueryGetData(&format)==S_OK)return true;
        }
        return false;
    }
    DWORD VisualEffect(DWORD effect,DWORD keys) {
        if(effect&&active_&&callbacks_.movingWithinApp) {
            try{if(callbacks_.movingWithinApp(callbacks_.context,paths_,keys))return DROPEFFECT_MOVE;}
            catch(...){}
        }
        return effect;
    }
    void Hover(bool hovering, POINT point) {
        if (!active_ || !callbacks_.hover) return;
        const DropCallbacks callbacks = callbacks_;
        try { callbacks.hover(callbacks.context, hovering, point); }
        catch (...) { /* A failed animation callback must not abort OLE. */ }
    }
    LONG references_ = 1;
    HWND window_=nullptr;
    IDropTargetHelper* helper_=nullptr;
    bool helperActive_=false;
    bool injectedHelper_=false;
    bool active_ = true;
    DropCallbacks callbacks_;
    std::vector<std::wstring> paths_;
};

class FormatEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEnumerator(std::vector<FORMATETC> formats, ULONG position = 0)
        : formats_(std::move(formats)), position_(position) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IEnumFORMATETC) return E_NOINTERFACE;
        *result = static_cast<IEnumFORMATETC*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* results, ULONG* fetched) override {
        if (!results || (!fetched && count != 1)) return E_POINTER;
        ULONG copied = 0;
        while (copied < count && position_ < formats_.size()) {
            results[copied++] = formats_[position_++];
        }
        if (fetched) *fetched = copied;
        return copied == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const ULONG available = static_cast<ULONG>(formats_.size()) - position_;
        position_ += (std::min)(count, available);
        return count <= available ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        try { *result = new FormatEnumerator(formats_, position_); }
        catch (...) { return E_OUTOFMEMORY; }
        return S_OK;
    }
private:
    LONG references_ = 1;
    std::vector<FORMATETC> formats_;
    ULONG position_ = 0;
};

HGLOBAL CopyGlobal(HGLOBAL memory) {
    const SIZE_T bytes = GlobalSize(memory);
    if (!bytes) return nullptr;
    const void* source = GlobalLock(memory);
    if (!source) return nullptr;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void* destination = copy ? GlobalLock(copy) : nullptr;
    if (destination) std::memcpy(destination, source, bytes);
    GlobalUnlock(memory);
    if (!destination) {
        if (copy) GlobalFree(copy);
        return nullptr;
    }
    GlobalUnlock(copy);
    return copy;
}

struct StoredMedium {
    FORMATETC format{};
    STGMEDIUM medium{};
    StoredMedium() = default;
    StoredMedium(const StoredMedium&) = delete;
    StoredMedium& operator=(const StoredMedium&) = delete;
    StoredMedium(StoredMedium&& other) noexcept : format(other.format), medium(other.medium) {
        other.medium = {};
    }
    StoredMedium& operator=(StoredMedium&& other) noexcept {
        if (this != &other) {
            ReleaseStgMedium(&medium);
            format = other.format;
            medium = other.medium;
            other.medium = {};
        }
        return *this;
    }
    ~StoredMedium() { ReleaseStgMedium(&medium); }
};

class FileReferenceData final : public IDataObject {
public:
    explicit FileReferenceData(const std::wstring& path) : path_(path) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *result = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        const HRESULT supported = QueryGetData(format);
        if (FAILED(supported)) return supported;
        HGLOBAL memory = nullptr;
        if (format->cfFormat == CF_HDROP) {
            const SIZE_T bytes = sizeof(DROPFILES) + (path_.size() + 2) * sizeof(wchar_t);
            memory = GlobalAlloc(GHND, bytes);
            if (!memory) return E_OUTOFMEMORY;
            BYTE* raw = static_cast<BYTE*>(GlobalLock(memory));
            if (!raw) { GlobalFree(memory); return E_OUTOFMEMORY; }
            auto* header = reinterpret_cast<DROPFILES*>(raw);
            header->pFiles = sizeof(DROPFILES);
            header->fWide = TRUE;
            std::memcpy(raw + sizeof(DROPFILES), path_.c_str(),
                        (path_.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
        } else if (format->cfFormat == PreferredEffectFormat()) {
            memory = GlobalAlloc(GHND, sizeof(DWORD));
            if (!memory) return E_OUTOFMEMORY;
            auto* effect = static_cast<DWORD*>(GlobalLock(memory));
            if (!effect) { GlobalFree(memory); return E_OUTOFMEMORY; }
            *effect = DROPEFFECT_COPY;
            GlobalUnlock(memory);
        } else {
            for (const auto& stored : stored_) {
                if (stored.format.cfFormat == format->cfFormat) {
                    memory = CopyGlobal(stored.medium.hGlobal);
                    break;
                }
            }
            if (!memory) return E_OUTOFMEMORY;
        }
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->lindex != -1) return DV_E_LINDEX;
        if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (format->cfFormat == CF_HDROP || format->cfFormat == PreferredEffectFormat()) return S_OK;
        for (const auto& stored : stored_) {
            if (stored.format.cfFormat == format->cfFormat) return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (output) output->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium) return E_POINTER;
        // Shell drag-image helpers attach private HGLOBAL formats through this
        // method. The exported reference and its safe preferred effect stay fixed.
        if (format->cfFormat == CF_HDROP || format->cfFormat == PreferredEffectFormat()) {
            return E_ACCESSDENIED;
        }
        if (format->ptd || format->dwAspect != DVASPECT_CONTENT || format->lindex != -1) {
            return DV_E_FORMATETC;
        }
        if (!(format->tymed & TYMED_HGLOBAL) || medium->tymed != TYMED_HGLOBAL ||
            !medium->hGlobal) return DV_E_TYMED;
        try {
            // Reserve before taking ownership: failure must leave the caller's
            // medium untouched, including when fRelease is TRUE.
            stored_.reserve(stored_.size() + 1);
            StoredMedium incoming;
            incoming.format = *format;
            incoming.format.tymed = TYMED_HGLOBAL;
            incoming.medium.tymed = TYMED_HGLOBAL;
            if (release) incoming.medium = *medium;
            else {
                incoming.medium.hGlobal = CopyGlobal(medium->hGlobal);
                if (!incoming.medium.hGlobal) return E_OUTOFMEMORY;
            }
            for (auto& stored : stored_) {
                if (stored.format.cfFormat == format->cfFormat) {
                    stored = std::move(incoming);
                    return S_OK;
                }
            }
            stored_.push_back(std::move(incoming));
        } catch (...) { return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        try {
            std::vector<FORMATETC> formats{FileFormat(),
                {PreferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
            for (const auto& stored : stored_) formats.push_back(stored.format);
            *result = new FormatEnumerator(std::move(formats));
        } catch (...) { return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** result) override {
        if (result) *result = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }
private:
    LONG references_ = 1;
    std::wstring path_;
    std::vector<StoredMedium> stored_;
};

bool IsDesktopWindow(HWND window) {
    if (!window || !IsWindow(window)) return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root) return false;
    wchar_t name[64]{};
    if (!GetClassNameW(root, name, static_cast<int>(sizeof(name) / sizeof(name[0])))) return false;
    return !_wcsicmp(name, L"Progman") || !_wcsicmp(name, L"WorkerW");
}

bool DesktopUnderCursor(void*) {
    POINT cursor{};
    return GetCursorPos(&cursor) && IsDesktopWindow(WindowFromPoint(cursor));
}

class FileReferenceSource final : public IDropSource {
public:
    explicit FileReferenceSource(bool* desktopDrop = nullptr,
        bool (*desktopDetector)(void*) = nullptr, void* detectorContext = nullptr,HWND owner=nullptr,
        bool (*beforeExternalDrop)(void*)=nullptr,void* externalContext=nullptr)
        : desktopDrop_(desktopDrop), desktopDetector_(desktopDetector ? desktopDetector : DesktopUnderCursor),
          detectorContext_(detectorContext),owner_(owner),beforeExternalDrop_(beforeExternalDrop),externalContext_(externalContext) {
        if (desktopDrop_) *desktopDrop_ = false;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDropSource) return E_NOINTERFACE;
        *result = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) {
            if (desktopDrop_) *desktopDrop_ = false;
            return DRAGDROP_S_CANCEL;
        }
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) {
            if (desktopDrop_) {
                bool desktop = false;
                try { desktop = desktopDetector_(detectorContext_); }
                catch (...) { /* Fall back to ordinary shell drop behavior. */ }
                *desktopDrop_ = desktop;
                // Cancel before OLE invokes the desktop target's Drop. The
                // caller restores its managed file exactly once after return.
                if (desktop) return DRAGDROP_S_CANCEL;
            }
            if(beforeExternalDrop_) {
                POINT cursor{};HWND root=nullptr;DWORD pid=0;wchar_t name[64]{};
                if(GetCursorPos(&cursor)){root=GetAncestor(WindowFromPoint(cursor),GA_ROOT);GetWindowThreadProcessId(root,&pid);GetClassNameW(root,name,64);}
                const bool card=pid==GetCurrentProcessId()&&!wcscmp(name,L"MintDesk.Card.1");
                if(!card){try{if(!beforeExternalDrop_(externalContext_))return DRAGDROP_S_CANCEL;}catch(...){return DRAGDROP_S_CANCEL;}}
            }
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
        if(owner_&&effect&&!(GetKeyState(VK_CONTROL)&0x8000)) {
            POINT p{};if(GetCursorPos(&p)) {
                HWND root=GetAncestor(WindowFromPoint(p),GA_ROOT);DWORD process=0;GetWindowThreadProcessId(root,&process);
                wchar_t name[64]{};GetClassNameW(root,name,64);
                if(process==GetCurrentProcessId()&&!wcscmp(name,L"MintDesk.Card.1")){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return S_OK;}
            }
        }
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
private:
    LONG references_ = 1;
    bool* desktopDrop_ = nullptr;
    bool (*desktopDetector_)(void*) = nullptr;
    void* detectorContext_ = nullptr;
    HWND owner_=nullptr;
    bool (*beforeExternalDrop_)(void*)=nullptr;
    void* externalContext_=nullptr;
};

void InitializeIconDragImage(IDataObject* data, HICON icon) {
    if (!icon) return;
    IDragSourceHelper* helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
        IID_IDragSourceHelper, reinterpret_cast<void**>(&helper)))) return;
    const int size = (std::max)(32, GetSystemMetrics(SM_CXICON));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (dc && pixels) {
        std::memset(pixels, 0, static_cast<std::size_t>(size) * size * 4);
        HGDIOBJ previous = SelectObject(dc, bitmap);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(dc, previous);
        // Icon pixels are premultiplied; the helper expects straight alpha.
        auto* colors = static_cast<DWORD*>(pixels);
        for (int index = 0; index < size * size; ++index) {
            const DWORD alpha = colors[index] >> 24;
            if (alpha && alpha < 255) {
                const DWORD blue = (std::min<DWORD>)(255, (colors[index] & 255u) * 255u / alpha);
                const DWORD green = (std::min<DWORD>)(255, ((colors[index] >> 8) & 255u) * 255u / alpha);
                const DWORD red = (std::min<DWORD>)(255, ((colors[index] >> 16) & 255u) * 255u / alpha);
                colors[index] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
        SHDRAGIMAGE image{};
        image.sizeDragImage = {size, size};
        image.ptOffset = {size / 2, size / 2};
        image.hbmpDragImage = bitmap;
        image.crColorKey = CLR_NONE;
        // The helper serializes the image into the object's private formats.
        helper->InitializeFromBitmap(&image, data);
    }
    if (dc) DeleteDC(dc);
    if (bitmap) DeleteObject(bitmap);
    helper->Release();
}

} // namespace

bool RegisterDrop(HWND window, const DropCallbacks& callbacks,IDropTargetHelper* helper) {
    if (!IsWindow(window) || GetPropW(window, kTargetProperty)) return false;
    auto* target = new (std::nothrow) FileDropTarget(window,callbacks,helper);
    if (!target) return false;
    if (!SetPropW(window, kTargetProperty, target)) { target->Release(); return false; }
    const HRESULT result = RegisterDragDrop(window, target);
    if (FAILED(result)) {
        RemovePropW(window, kTargetProperty);
        target->Deactivate();
        target->Release();
        return false;
    }
    // The property retains the initial reference; OLE owns a separate one.
    return true;
}
bool RegisterFileDrop(HWND window,const DropCallbacks& callbacks){return RegisterDrop(window,callbacks,nullptr);}

void RevokeFileDrop(HWND window) {
    auto* target = static_cast<FileDropTarget*>(RemovePropW(window, kTargetProperty));
    if (!target) return;
    // Disable callbacks before OLE pumps or releases anything. An in-flight
    // DragOver/Drop can then finish without touching a destroyed card/context.
    target->Deactivate();
    RevokeDragDrop(window);
    target->Release();
}

DWORD DragFileReference(HWND owner, const std::wstring& path, HICON icon, bool* droppedOnDesktop,
    bool (*beforeExternalDrop)(void*),void* externalContext) {
    if (droppedOnDesktop) *droppedOnDesktop = false;
    IDataObject* data = testing::CreateFileReferenceDataObject(path);
    if (!data) return DROPEFFECT_NONE;
    IDropSource* source = new (std::nothrow) FileReferenceSource(droppedOnDesktop,nullptr,nullptr,owner,beforeExternalDrop,externalContext);
    if (!source) { data->Release(); return DROPEFFECT_NONE; }
    InitializeIconDragImage(data, icon);
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(data, source,
        DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    source->Release();
    data->Release();
    return result == DRAGDROP_S_DROP ? effect & (DROPEFFECT_COPY | DROPEFFECT_LINK) : DROPEFFECT_NONE;
}

namespace testing {
bool RegisterFileDropWithImageHelper(HWND window,const DropCallbacks& callbacks,IDropTargetHelper* helper){return RegisterDrop(window,callbacks,helper);}
IDropTarget* AcquireRegisteredFileDropTarget(HWND window) {
    auto* target = static_cast<FileDropTarget*>(GetPropW(window, kTargetProperty));
    if (target) target->AddRef();
    return target;
}
IDataObject* CreateFileReferenceDataObject(const std::wstring& path) {
    if (!IsSupportedPath(path)) return nullptr;
    try { return new FileReferenceData(path); }
    catch (...) { return nullptr; }
}
bool IsDesktopDropWindow(HWND window) {
    return IsDesktopWindow(window);
}
IDropSource* CreateFileReferenceDropSource(bool* droppedOnDesktop,
    bool (*desktopDetector)(void*), void* detectorContext,bool (*beforeExternalDrop)(void*),void* externalContext) {
    return new (std::nothrow) FileReferenceSource(droppedOnDesktop, desktopDetector, detectorContext,nullptr,beforeExternalDrop,externalContext);
}
} // namespace testing
} // namespace desk
