#include "../src/dragdrop.h"
#include "../src/shell_items.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); ++failures; }
}

HGLOBAL FileList(const std::vector<std::wstring>& paths, bool terminate = true) {
    std::vector<wchar_t> names;
    for (const auto& path : paths) {
        names.insert(names.end(), path.begin(), path.end());
        names.push_back(L'\0');
    }
    names.push_back(terminate ? L'\0' : L'x');
    const SIZE_T bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return nullptr;
    BYTE* raw = static_cast<BYTE*>(GlobalLock(memory));
    if (!raw) { GlobalFree(memory); return nullptr; }
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(raw, &header, sizeof(header));
    std::memcpy(raw + sizeof(header), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    return memory;
}

// A custom pUnkForRelease verifies that incoming STGMEDIUM ownership is honored.
class MediumOwner final : public IUnknown {
public:
    MediumOwner(HGLOBAL memory, int* released) : memory_(memory), released_(released) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown) return E_NOINTERFACE;
        *result = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
private:
    ~MediumOwner() { GlobalFree(memory_); ++*released_; }
    ULONG references_ = 1;
    HGLOBAL memory_;
    int* released_;
};

class IncomingData final : public IDataObject {
public:
    std::vector<std::wstring> paths;
    int reads = 0;
    int released = 0;
    bool unsupported = false;
    bool terminate = true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *result = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium || !format) return E_POINTER;
        if(format->cfFormat==CF_HDROP)++reads;
        *medium = {};
        if (unsupported || format->cfFormat != CF_HDROP || !(format->tymed & TYMED_HGLOBAL)) {
            return DV_E_FORMATETC;
        }
        HGLOBAL memory = FileList(paths, terminate);
        if (!memory) return E_OUTOFMEMORY;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = new MediumOwner(memory, &released);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return !unsupported && format && format->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    ULONG references_ = 1;
};

struct Callbacks {
    HWND window = nullptr;
    int entered = 0;
    int left = 0;
    int dropped = 0;
    bool accept = true;
    bool destroyed = false;
    bool revokeOnHover = false;
    bool revokeOnDrop = false;
    bool revokeOnLeave = false;
    POINT last{};
    std::vector<std::wstring> paths;
    const bool* imageHidden=nullptr;
};
void Hover(void* raw, bool hovered, POINT point) {
    auto& context = *static_cast<Callbacks*>(raw);
    Check(!context.destroyed, "no callback touches a revoked context");
    if (hovered) ++context.entered;
    else ++context.left;
    context.last = point;
    if ((hovered && context.revokeOnHover) || (!hovered && context.revokeOnLeave)) {
        desk::RevokeFileDrop(context.window);
        context.destroyed = true;
    }
}
bool Drop(void* raw, const std::vector<std::wstring>& paths, POINT point,DWORD) {
    auto& context = *static_cast<Callbacks*>(raw);
    Check(!context.destroyed, "drop does not touch a revoked context");
    if(context.imageHidden)Check(*context.imageHidden,"source drag image is hidden before the application handles files");
    ++context.dropped;
    context.paths = paths;
    context.last = point;
    if (context.revokeOnDrop) {
        desk::RevokeFileDrop(context.window);
        context.destroyed = true;
    }
    return context.accept;
}
desk::DropCallbacks Hooks(Callbacks& context) { return {&context, Hover, Drop}; }

class ImageHelper final:public IDropTargetHelper {
public:
    LONG refs=1;int enters=0,overs=0,leaves=0,drops=0;bool hidden=false;DWORD lastEffect=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(id!=IID_IUnknown&&id!=IID_IDropTargetHelper)return E_NOINTERFACE;*out=static_cast<IDropTargetHelper*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const LONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE DragEnter(HWND,IDataObject*,POINT*,DWORD effect) override{++enters;hidden=false;lastEffect=effect;return S_OK;}
    HRESULT STDMETHODCALLTYPE DragLeave() override{++leaves;hidden=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE DragOver(POINT*,DWORD effect) override{++overs;lastEffect=effect;return S_OK;}
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*,POINT*,DWORD effect) override{++drops;hidden=true;lastEffect=effect;return S_OK;}
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override{hidden=!show;return S_OK;}
};

HWND Window() {
    return CreateWindowExW(0, L"STATIC", L"MintDesk isolated OLE test",
        WS_POPUP, 0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
void TestImageLifecycle(const std::wstring& fixture) {
    HWND window=Window();Callbacks context;context.window=window;
    auto* image=new ImageHelper;context.imageHidden=&image->hidden;
    Check(desk::testing::RegisterFileDropWithImageHelper(window,Hooks(context),image),"register isolated drag-image helper spy");
    IDropTarget* target=desk::testing::AcquireRegisteredFileDropTarget(window);
    IncomingData data;data.paths={fixture};DWORD effect=DROPEFFECT_COPY;
    if(target) {
        target->DragEnter(&data,MK_LBUTTON,{0,0},&effect);
        Check(image->enters==1&&!image->hidden,"drag enter starts image feedback");
        effect=DROPEFFECT_COPY;target->DragOver(MK_LBUTTON,{1,1},&effect);
        Check(image->overs==1,"drag-over updates image position");
        effect=DROPEFFECT_COPY;target->Drop(&data,0,{1,1},&effect);
        Check(context.dropped==1&&image->hidden&&image->drops==1&&effect==DROPEFFECT_COPY,"drop ends the image and reports the committed result");
        effect=DROPEFFECT_COPY;target->DragEnter(&data,0,{0,0},&effect);target->DragLeave();
        Check(image->hidden&&image->leaves==1,"cancelled drag removes its image");
        effect=DROPEFFECT_COPY;target->DragEnter(&data,0,{0,0},&effect);
        desk::RevokeFileDrop(window);
        Check(image->hidden&&image->leaves==2,"revoking a target while dragging clears the system image");
        target->Release();
    }
    DestroyWindow(window);Check(image->refs==1,"drop target releases its drag-image helper ownership");image->Release();
}

void TestTarget(const std::wstring& fixture) {
    HWND window = Window();
    Check(window != nullptr, "create isolated hidden target window");
    if (!window) return;
    HANDLE sentinel = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0x1234));
    Check(SetPropW(window, L"Unrelated.fixture", sentinel) != FALSE, "set unrelated property");
    Callbacks context;
    context.window = window;
    Check(desk::RegisterFileDrop(window, Hooks(context)), "register native OLE target");
    Check(!desk::RegisterFileDrop(window, Hooks(context)), "duplicate registration is rejected safely");
    IDropTarget* target = desk::testing::AcquireRegisteredFileDropTarget(window);
    Check(target != nullptr, "registered target is retained for testing");
    if (!target) { DestroyWindow(window); return; }
    IncomingData input;
    input.paths = {fixture, L"relative.txt", L"https://example.invalid", L"C:\\safe\\second.lnk"};
    const std::vector<std::wstring> expected{fixture, L"C:\\safe\\second.lnk"};
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    Check(target->DragEnter(&input, MK_LBUTTON, {123, 456}, &effect) == S_OK &&
        effect == DROPEFFECT_COPY, "incoming file drag offers copy instead of move");
    Check(context.entered == 1 && context.last.x == 123 && context.last.y == 456,
        "hover animation receives screen coordinates");
    Check(input.reads == 1 && input.released == 1, "incoming medium is released exactly once");
    for (int i = 0; i < 20; ++i) {
        effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        target->DragOver(MK_LBUTTON, {123 + i, 456}, &effect);
        Check(effect == DROPEFFECT_COPY, "drag over retains safe copy effect");
    }
    Check(input.reads == 1, "drag over uses cached paths without another GetData");
    effect = DROPEFFECT_COPY | DROPEFFECT_LINK | DROPEFFECT_MOVE;
    target->DragOver(MK_CONTROL | MK_SHIFT | MK_LBUTTON, {140, 460}, &effect);
    Check(effect == DROPEFFECT_LINK, "Control plus Shift requests supported link effect");
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    Check(target->Drop(&input, 0, {160, 480}, &effect) == S_OK && effect == DROPEFFECT_COPY,
        "accepted reference import reports copy");
    Check(context.dropped == 1 && context.paths == expected && context.left == 1,
        "drop filters unsupported paths and ends the hover animation");
    Check(input.reads == 1, "drop also reuses the entered paths");

    effect = DROPEFFECT_MOVE;
    target->DragEnter(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE, "move-only source is never accepted as a move");
    effect = DROPEFFECT_MOVE;
    target->Drop(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE && context.dropped == 1, "move-only drop does not import");

    context.accept = false;
    effect = DROPEFFECT_LINK;
    target->DragEnter(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_LINK, "link-only source is supported");
    effect = DROPEFFECT_LINK;
    target->Drop(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE && context.dropped == 2, "rejected import reports no effect");

    const int beforeUnsupported = context.entered;
    input.unsupported = true;
    effect = DROPEFFECT_COPY;
    target->DragEnter(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE && context.entered == beforeUnsupported,
        "unsupported data does not highlight the target");
    input.unsupported = false;
    input.terminate = false;
    effect = DROPEFFECT_COPY;
    target->DragEnter(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE, "unterminated file drop is rejected");
    Check(input.released == input.reads - 1, "rejected and accepted media are all released");
    input.terminate = true;

    desk::RevokeFileDrop(window);
    context.destroyed = true;
    Check(desk::testing::AcquireRegisteredFileDropTarget(window) == nullptr,
        "revoking removes only the owned target registration");
    Check(GetPropW(window, L"Unrelated.fixture") == sentinel, "other window properties survive revoke");
    effect = DROPEFFECT_COPY;
    target->DragEnter(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE, "an externally retained revoked target stays inactive");
    effect = DROPEFFECT_COPY;
    target->DragOver(0, {0, 0}, &effect);
    target->DragLeave();
    target->Drop(&input, 0, {0, 0}, &effect);
    Check(effect == DROPEFFECT_NONE, "revoked callbacks remain disabled through all COM methods");
    desk::RevokeFileDrop(window);
    target->Release();
    RemovePropW(window, L"Unrelated.fixture");
    DestroyWindow(window);
}

void TestReentrantRevoke(const std::wstring& fixture) {
    for (int stage = 0; stage < 3; ++stage) {
        HWND window = Window();
        Callbacks context;
        context.window = window;
        context.revokeOnHover = stage == 0;
        context.revokeOnLeave = stage == 1;
        context.revokeOnDrop = stage == 2;
        Check(desk::RegisterFileDrop(window, Hooks(context)), "register reentrant test target");
        IDropTarget* target = desk::testing::AcquireRegisteredFileDropTarget(window);
        if (!target) { DestroyWindow(window); continue; }
        IncomingData input;
        input.paths = {fixture};
        DWORD effect = DROPEFFECT_COPY;
        target->DragEnter(&input, 0, {0, 0}, &effect);
        effect = DROPEFFECT_COPY;
        target->Drop(&input, 0, {0, 0}, &effect);
        Check(context.destroyed, "callbacks can revoke registration reentrantly");
        Check(context.dropped == (stage == 2 ? 1 : 0),
            "revocation during hover or leave suppresses following drop callback");
        target->DragLeave();
        target->Release();
        DestroyWindow(window);
    }
}

void TestOutbound(const std::wstring& fixture) {
    IDataObject* data = desk::testing::CreateFileReferenceDataObject(fixture);
    Check(data != nullptr, "create outbound Unicode reference data object");
    if (!data) return;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM first{}, second{};
    Check(data->GetData(&format, &first) == S_OK, "outbound CF_HDROP format is readable");
    Check(data->GetData(&format, &second) == S_OK && first.hGlobal != second.hGlobal,
        "each caller receives independently owned file-drop storage");
    Check(desk::PathsFromDrop(reinterpret_cast<HDROP>(first.hGlobal)) ==
        std::vector<std::wstring>{fixture}, "outbound object exports exactly its original Unicode path");
    ReleaseStgMedium(&first);
    ReleaseStgMedium(&second);
    format.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    STGMEDIUM preferred{};
    Check(data->GetData(&format, &preferred) == S_OK, "outbound preferred effect is available");
    auto* effect = static_cast<DWORD*>(GlobalLock(preferred.hGlobal));
    Check(effect && *effect == DROPEFFECT_COPY, "outbound preferred effect never requests move");
    if (effect) GlobalUnlock(preferred.hGlobal);
    ReleaseStgMedium(&preferred);
    format.cfFormat = CF_TEXT;
    Check(data->QueryGetData(&format) == DV_E_FORMATETC, "unoffered format is rejected");
    format.cfFormat = CF_HDROP;
    format.tymed = TYMED_ISTREAM;
    Check(data->QueryGetData(&format) == DV_E_TYMED, "unsupported storage type is rejected");
    format.tymed = TYMED_HGLOBAL;
    format.lindex = 0;
    Check(data->QueryGetData(&format) == DV_E_LINDEX, "unsupported file index is rejected");

    IEnumFORMATETC* enumerator = nullptr;
    Check(data->EnumFormatEtc(DATADIR_GET, &enumerator) == S_OK && enumerator,
        "outbound object exposes format enumeration");
    if (enumerator) {
        FORMATETC formats[3]{};
        ULONG fetched = 0;
        Check(enumerator->Next(3, formats, &fetched) == S_FALSE && fetched == 2 &&
            formats[0].cfFormat == CF_HDROP, "enumeration offers file drop and preferred effect");
        Check(enumerator->Reset() == S_OK && enumerator->Skip(1) == S_OK,
            "format enumerator supports reset and skip");
        IEnumFORMATETC* clone = nullptr;
        Check(enumerator->Clone(&clone) == S_OK && clone, "format enumerator can clone its position");
        if (clone) {
            Check(clone->Next(1, formats, nullptr) == S_OK && formats[0].cfFormat ==
                RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), "cloned enumerator retains position");
            clone->Release();
        }
        enumerator->Release();
    }

    FORMATETC imageFormat{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"MintDesk-Test-DragImage")),
        nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM image{};
    image.tymed = TYMED_HGLOBAL;
    image.hGlobal = GlobalAlloc(GHND, sizeof(DWORD));
    DWORD* value = static_cast<DWORD*>(GlobalLock(image.hGlobal));
    *value = 0x12345678;
    GlobalUnlock(image.hGlobal);
    Check(data->SetData(&imageFormat, &image, FALSE) == S_OK,
        "native drag image can store private data without transferring ownership");
    ReleaseStgMedium(&image);
    STGMEDIUM copied{};
    Check(data->GetData(&imageFormat, &copied) == S_OK, "private drag image medium is copied");
    value = static_cast<DWORD*>(GlobalLock(copied.hGlobal));
    Check(value && *value == 0x12345678, "private image storage remains valid after source medium release");
    if (value) GlobalUnlock(copied.hGlobal);
    ReleaseStgMedium(&copied);
    image.hGlobal = GlobalAlloc(GHND, sizeof(DWORD));
    image.tymed = TYMED_HGLOBAL;
    Check(data->SetData(&imageFormat, &image, TRUE) == S_OK,
        "private drag image medium can transfer ownership");
    // The object now owns image; do not ReleaseStgMedium it a second time.
    format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Check(data->SetData(&format, &image, FALSE) == E_ACCESSDENIED,
        "target cannot replace the exported source reference");
    data->Release();
    Check(!desk::testing::CreateFileReferenceDataObject(L"https://example.invalid"),
        "outbound URL commands are rejected");
    Check(desk::DragFileReference(nullptr, L"relative.exe") == DROPEFFECT_NONE,
        "invalid outbound reference returns without opening a modal drag loop");

    IDropSource* source = desk::testing::CreateFileReferenceDropSource();
    Check(source != nullptr, "create native outbound drop source");
    if (source) {
        Check(source->QueryContinueDrag(TRUE, MK_LBUTTON) == DRAGDROP_S_CANCEL,
            "Escape cancels the native drag");
        Check(source->QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK,
            "held mouse button continues dragging");
        Check(source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP,
            "mouse release completes dragging");
        Check(source->GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS,
            "native cursors provide operation feedback");
        source->Release();
    }
}

struct DesktopDetector {
    int calls = 0;
    bool desktop = false;
};
bool DetectDesktop(void* raw) {
    auto& detector = *static_cast<DesktopDetector*>(raw);
    ++detector.calls;
    return detector.desktop;
}

struct ExternalDrop {int calls=0;bool allowed=true;};
bool PrepareExternal(void* raw){auto& state=*static_cast<ExternalDrop*>(raw);++state.calls;return state.allowed;}
void TestDesktopInterception() {
    ExternalDrop external;
    IDropSource* guarded=desk::testing::CreateFileReferenceDropSource(nullptr,nullptr,nullptr,PrepareExternal,&external);
    if(guarded){
        Check(guarded->QueryContinueDrag(FALSE,MK_LBUTTON)==S_OK&&external.calls==0,"external preparation waits until release");
        Check(guarded->QueryContinueDrag(TRUE,0)==DRAGDROP_S_CANCEL&&external.calls==0,"Escape leaves source visibility untouched");
        Check(guarded->QueryContinueDrag(FALSE,0)==DRAGDROP_S_DROP&&external.calls==1,"external source visibility is prepared before the target receives Drop");
        external.allowed=false;
        Check(guarded->QueryContinueDrag(FALSE,0)==DRAGDROP_S_CANCEL&&external.calls==2,"failed visibility restoration cancels external copy safely");
        guarded->Release();
    }
    DesktopDetector detector;
    detector.desktop = true;
    bool desktop = true;
    IDropSource* source = desk::testing::CreateFileReferenceDropSource(&desktop, DetectDesktop, &detector);
    Check(source != nullptr && !desktop, "opted-in drag initializes the desktop result to false");
    if (source) {
        Check(source->QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK && detector.calls == 0 && !desktop,
            "holding the drag button never intercepts or queries the desktop");
        Check(source->QueryContinueDrag(FALSE, MK_RBUTTON) == S_OK && detector.calls == 0,
            "holding either drag button continues normally");
        Check(source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL && desktop && detector.calls == 1,
            "managed desktop drop cancels OLE before Explorer can copy its source");
        source->Release();
    }

    desktop = true;
    source = desk::testing::CreateFileReferenceDropSource(&desktop, DetectDesktop, &detector);
    if (source) {
        const int beforeEscape = detector.calls;
        Check(source->QueryContinueDrag(TRUE, 0) == DRAGDROP_S_CANCEL && !desktop &&
            detector.calls == beforeEscape, "Escape cancellation never requests desktop restoration");
        source->Release();
    }

    detector.desktop = false;
    source = desk::testing::CreateFileReferenceDropSource(&desktop, DetectDesktop, &detector);
    if (source) {
        Check(source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP && !desktop,
            "ordinary Explorer-folder drop retains native OLE completion");
        source->Release();
    }

    detector.desktop = true;
    source = desk::testing::CreateFileReferenceDropSource(nullptr, DetectDesktop, &detector);
    if (source) {
        const int beforeUnmanaged = detector.calls;
        Check(source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP &&
            detector.calls == beforeUnmanaged, "unmanaged references never opt into desktop interception");
        source->Release();
    }

    desktop = true;
    Check(desk::DragFileReference(nullptr, L"relative.exe", nullptr, &desktop) == DROPEFFECT_NONE && !desktop,
        "invalid drag clears the optional desktop result without entering OLE");

    // All windows are hidden and belong to this process. Their registered class
    // names exercise the same root-window classification used in production.
    const wchar_t* classes[] = {L"Progman", L"WorkerW", L"CabinetWClass"};
    for (int index = 0; index < 3; ++index) {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = classes[index];
        const ATOM atom = RegisterClassW(&type);
        Check(atom != 0, "register isolated shell-class fixture");
        if (!atom) continue;
        HWND parent = CreateWindowExW(0, classes[index], L"Hidden desktop classification fixture",
            WS_POPUP, 0, 0, 20, 20, nullptr, nullptr, type.hInstance, nullptr);
        HWND child = parent ? CreateWindowExW(0, L"STATIC", L"Hidden desktop child",
            WS_CHILD, 0, 0, 10, 10, parent, nullptr, type.hInstance, nullptr) : nullptr;
        Check(parent != nullptr && child != nullptr, "create isolated root and child windows");
        if (parent && child) {
            Check(desk::testing::IsDesktopDropWindow(parent) == (index < 2),
                "desktop root classes are distinguished from Explorer folder roots");
            Check(desk::testing::IsDesktopDropWindow(child) == (index < 2),
                "desktop child classification follows the actual root window");
        }
        if (parent) DestroyWindow(parent);
        UnregisterClassW(classes[index], type.hInstance);
    }
    Check(!desk::testing::IsDesktopDropWindow(nullptr), "an absent drop window is not the desktop");
}
} // namespace

int main() {
    const HRESULT initialized = OleInitialize(nullptr);
    Check(SUCCEEDED(initialized), "initialize the test UI thread for OLE");
    if (FAILED(initialized)) return 1;
    wchar_t temporary[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temporary) != 0, "locate local temporary test directory");
    const std::wstring directory = std::wstring(temporary) + L"MintDesk-dragdrop-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Check(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "create isolated local fixture directory");
    const std::wstring fixture = directory + L"\\\u5de5\u5177 \U0001F4BB.txt";
    HANDLE file = CreateFileW(fixture.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create isolated Unicode local fixture");
    if (file != INVALID_HANDLE_VALUE) {
        const char sentinel[] = "MintDesk original file remains unchanged";
        DWORD written = 0;
        WriteFile(file, sentinel, sizeof(sentinel), &written, nullptr);
        CloseHandle(file);
        TestTarget(fixture);
        TestImageLifecycle(fixture);
        TestReentrantRevoke(fixture);
        TestOutbound(fixture);
        TestDesktopInterception();
        file = CreateFileW(fixture.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "drag operations preserve the source file");
        if (file != INVALID_HANDLE_VALUE) {
            char actual[sizeof(sentinel)]{};
            DWORD read = 0;
            Check(ReadFile(file, actual, sizeof(actual), &read, nullptr) && read == sizeof(sentinel) &&
                std::memcmp(actual, sentinel, sizeof(sentinel)) == 0,
                "source file bytes remain unchanged");
            CloseHandle(file);
        }
        DeleteFileW(fixture.c_str());
    }
    RemoveDirectoryW(directory.c_str());
    OleUninitialize();
    if (!failures) std::puts("Native drag-and-drop tests passed.");
    return failures ? 1 : 0;
}
