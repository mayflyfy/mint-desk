#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleidl.h>

#include <string>
#include <vector>
struct IDropTargetHelper;

namespace desk {

struct DropCallbacks {
    void* context = nullptr;
    void (*hover)(void*, bool, POINT) = nullptr;
    bool (*drop)(void*, const std::vector<std::wstring>&, POINT, DWORD keys) = nullptr;
    bool (*movingWithinApp)(void*, const std::vector<std::wstring>&, DWORD keys) = nullptr;
};

// Call on the window's OLE-initialized UI thread. Revoke before destroying its
// callback context. Coordinates supplied to callbacks are screen coordinates.
bool RegisterFileDrop(HWND window, const DropCallbacks& callbacks);
void RevokeFileDrop(HWND window);

// Exports the existing file reference through Windows drag and drop. Only COPY
// and LINK are offered; this function never moves or deletes the source file.
// The caller keeps its card/context alive until this modal operation returns.
// If opted in, a release over the desktop cancels OLE before Explorer receives
// the file and sets droppedOnDesktop=true. The caller can restore its managed
// item itself; the returned OLE effect is NONE for this intercepted operation.
DWORD DragFileReference(HWND owner, const std::wstring& path, HICON icon = nullptr,
                        bool* droppedOnDesktop = nullptr,
                        bool (*beforeExternalDrop)(void*) = nullptr,void* externalContext = nullptr);

namespace testing {
// Each non-null return owns one reference, to be released by the caller.
IDropTarget* AcquireRegisteredFileDropTarget(HWND window);
bool RegisterFileDropWithImageHelper(HWND window,const DropCallbacks& callbacks,IDropTargetHelper* helper);
IDataObject* CreateFileReferenceDataObject(const std::wstring& path);
bool IsDesktopDropWindow(HWND window);
IDropSource* CreateFileReferenceDropSource(bool* droppedOnDesktop = nullptr,
    bool (*desktopDetector)(void*) = nullptr, void* detectorContext = nullptr,
    bool (*beforeExternalDrop)(void*) = nullptr,void* externalContext = nullptr);
} // namespace testing

} // namespace desk
