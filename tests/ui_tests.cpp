// Exercise actual window procedures in preview mode. The close-failure test
// briefly exercises production control flow, with every layout write redirected
// to an explicit temporary directory and desktop-menu refreshes intercepted.
#include "../src/state.h"
#include "../src/desktop_menu.h"
namespace desk {
bool SaveUiTestState(const AppState&);
bool RefreshUiTestDesktopMenu(bool);
}
namespace {
BOOL UiTestCursor(LPPOINT);
SHORT UiTestAsyncKey(int);
}
#define SaveState SaveUiTestState
#define RefreshDesktopMenu RefreshUiTestDesktopMenu
#define GetCursorPos UiTestCursor
#define GetAsyncKeyState UiTestAsyncKey
#define wWinMain MintDeskAppEntry
#include "../src/main.cpp"
#undef wWinMain
#undef RefreshDesktopMenu
#undef SaveState
#undef GetCursorPos
#undef GetAsyncKeyState

#include <cstdio>
#include <cstdlib>
#include <gdiplus.h>

namespace {
std::wstring uiTestStateDirectory;
unsigned uiTestSaveCalls = 0, uiTestMenuRefreshCalls = 0;
bool uiTestPointerInjected=false;
POINT uiTestPointer{};
BOOL UiTestCursor(LPPOINT point) {
    if(uiTestPointerInjected){*point=uiTestPointer;return TRUE;}
    return ::GetCursorPos(point);
}
SHORT UiTestAsyncKey(int key) {
    if(uiTestPointerInjected)return key==VK_LBUTTON?static_cast<SHORT>(0x8000):0;
    return ::GetAsyncKeyState(key);
}
}
namespace desk {
bool SaveUiTestState(const AppState& state) {
    ++uiTestSaveCalls;
    return !uiTestStateDirectory.empty() &&
        testing::SaveStateToDirectory(uiTestStateDirectory, state);
}
bool RefreshUiTestDesktopMenu(bool) {
    ++uiTestMenuRefreshCalls;
    return true;
}
}

namespace {
int testFailures = 0;

void CheckUi(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++testFailures;
    }
}

RECT WindowRect(HWND window) {
    RECT rect{};
    CheckUi(GetWindowRect(window, &rect) != FALSE, "get native window rectangle");
    return rect;
}

bool EqualRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

LRESULT HitAt(Card& card, int x, int y) {
    POINT point{x, y};
    ClientToScreen(card.hwnd, &point);
    return SendMessageW(card.hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
}

bool IconAlive(HICON icon) {
    ICONINFO info{};
    const bool alive = GetIconInfo(icon, &info) != FALSE;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return alive;
}

HICON TestIcon() {
    HICON icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    CheckUi(icon != nullptr, "create owned test icon");
    return icon;
}

uint32_t OpaquePixel(COLORREF color) {
    return 0xff000000u | (static_cast<uint32_t>(GetRValue(color)) << 16) |
        (static_cast<uint32_t>(GetGValue(color)) << 8) | GetBValue(color);
}

HICON SolidTestIcon(COLORREF color) {
    constexpr int size = 16;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    const BYTE transparentMask[size * 2]{};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, transparentMask);
    HICON icon = nullptr;
    if (bitmap && bits && mask) {
        std::fill_n(static_cast<uint32_t*>(bits), size * size, OpaquePixel(color));
        ICONINFO iconInfo{};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmColor = bitmap;
        iconInfo.hbmMask = mask;
        icon = CreateIconIndirect(&iconInfo);
    }
    if (mask) DeleteObject(mask);
    if (bitmap) DeleteObject(bitmap);
    CheckUi(icon != nullptr, "create deterministic opaque native icon");
    return icon;
}

std::vector<uint32_t> SurfacePixels(Card& card, int& width, int& height) {
    desk::AlphaSurface surface;
    const bool rendered = RenderCardSurface(card, surface);
    CheckUi(rendered && surface.Pixels(), "render production premultiplied card surface");
    width = surface.Width();
    height = surface.Height();
    if (!rendered || !surface.Pixels()) return {};
    return {surface.Pixels(), surface.Pixels() + static_cast<size_t>(width) * height};
}

std::vector<uint32_t> SurfaceRegion(Card& card, const RECT& region) {
    int width = 0, height = 0;
    const auto pixels = SurfacePixels(card, width, height);
    std::vector<uint32_t> result;
    if (pixels.empty()) return result;
    CheckUi(region.left >= 0 && region.top >= 0 && region.right <= width && region.bottom <= height,
        "requested render comparison stays inside the native client surface");
    if (region.left < 0 || region.top < 0 || region.right > width || region.bottom > height) return result;
    for (int y = region.top; y < region.bottom; ++y)
        result.insert(result.end(), pixels.begin() + static_cast<size_t>(y) * width + region.left,
            pixels.begin() + static_cast<size_t>(y) * width + region.right);
    return result;
}

COLORREF PaintedPixel(Card& card, POINT point) {
    RECT client{};
    GetClientRect(card.hwnd, &client);
    HDC screen = GetDC(nullptr);
    HDC canvas = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, Width(client), Height(client));
    ReleaseDC(nullptr, screen);
    COLORREF color = CLR_INVALID;
    CheckUi(canvas && bitmap, "allocate native feedback rendering surface");
    if (canvas && bitmap) {
        HGDIOBJ previous = SelectObject(canvas, bitmap);
        Paint(card, canvas);
        color = GetPixel(canvas, point.x, point.y);
        SelectObject(canvas, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (canvas) DeleteDC(canvas);
    return color;
}

struct Fixture {
    std::wstring directory;
    std::wstring original;
    bool ready = false;
    Fixture() {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporary)) return;
        directory = std::wstring(temporary) + L"MintDesk-ui-test-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        if (!CreateDirectoryW(directory.c_str(), nullptr)) return;
        original = directory + L"\\original-\u6587\u4ef6.txt";
        HANDLE file = CreateFileW(original.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const char content[] = "MintDesk integration fixture\n";
        DWORD written = 0;
        ready = WriteFile(file, content, sizeof(content) - 1, &written, nullptr) &&
            written == sizeof(content) - 1;
        CloseHandle(file);
    }
    ~Fixture() {
        if (!original.empty()) DeleteFileW(original.c_str());
        if (!directory.empty()) RemoveDirectoryW(directory.c_str());
    }
    bool originalExists() const {
        return GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
};

void TestSelectionDismissal(const Fixture& fixture) {
    desk::CardState state; state.id=app.nextId++; state.title=L"selection fixture";
    state.layout=desk::Layout::List; state.paths={fixture.original};
    Card* first=CreateCard(state); state.id=app.nextId++;
    Card* second=CreateCard(state);
    CheckUi(first && second,"create independent selection fixtures");
    if (first && second) {
        const auto click=[](Card& card) {
            const RECT item=ItemRect(card,0); const LPARAM point=MAKELPARAM(item.left+5,item.top+5);
            SendMessageW(card.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,point);
            SendMessageW(card.hwnd,WM_LBUTTONUP,0,point);
        };
        click(*first); CheckUi(first->selected==0 && app.selectionHook,"click selects a card item and enables its event-only click watcher");
        SendMessageW(app.controller,kSelectionClick,reinterpret_cast<WPARAM>(GetDesktopWindow()),0);
        CheckUi(first->selected==-1 && !app.selectionHook,
            "desktop click clears selection even without activation/focus changes and removes the idle watcher");
        click(*first);
        click(*second);
        CheckUi(first->selected==-1 && second->selected==0,"clicking another card clears the old selection");
        SendMessageW(first->hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
        CheckUi(second->selected==-1,"clicking another card title also clears selection");
        click(*first);
        SendMessageW(first->hwnd,WM_ACTIVATE,WA_INACTIVE,reinterpret_cast<LPARAM>(GetDesktopWindow()));
        CheckUi(first->selected==-1,"desktop activation clears the selected highlight");
        first->selected=0;
        SendMessageW(first->hwnd,WM_KILLFOCUS,reinterpret_cast<WPARAM>(GetDesktopWindow()),0);
        CheckUi(first->selected==-1,"external focus clears selection without another card click");
        first->selected=0; second->selected=0;
        SendMessageW(first->hwnd,WM_ACTIVATEAPP,FALSE,0);
        CheckUi(first->selected==-1 && second->selected==-1,"switching away clears every card selection");
        first->selected=0; BeginRename(*first);
        CheckUi(first->editor && first->selected==0,"title editor focus does not lose the card selection");
        FinishRename(*first,false);
        first->selected=0;
        { ModalScope modal;
            SendMessageW(first->hwnd,WM_KILLFOCUS,0,0);
            CheckUi(first->selected==0,"native popup menu focus retains its selected command target");
            SendMessageW(app.controller,kSelectionClick,reinterpret_cast<WPARAM>(GetDesktopWindow()),0);
            CheckUi(first->selected==0 && app.pendingSelectionClick,"popup click defers dismissal until its command can use the selected item");
        }
        MSG deferred{};
        if(PeekMessageW(&deferred,app.controller,kSelectionClick,kSelectionClick,PM_REMOVE))DispatchMessageW(&deferred);
        CheckUi(first->selected==-1 && !app.selectionHook,"deferred outside click clears selection after the native popup completes");
    }
    if(second) HandleCommand(second,RemoveCard);
    if(first) HandleCommand(first,RemoveCard);
}

void TestSavedUserLayout(const Fixture& fixture) {
    const auto monitors=desk::EnumerateMonitors(); if(monitors.empty())return;
    const auto userRoot=fixture.directory+L"\\UserData";
    CheckUi(CreateDirectoryW(userRoot.c_str(),nullptr)!=FALSE,"create isolated user-data root");
    const auto data=desk::testing::DataDirectoryUnder(userRoot);
    desk::AppState saved; saved.nextId=app.nextId+3;
    for(int i=0;i<3;++i) {
        desk::CardState state;state.id=app.nextId++;state.title=L"saved user card "+std::to_wstring(i);
        state.monitor=monitors.front().device;state.x=3+i*310;state.y=4;
        state.width=300;state.height=250;state.locked=i==1;
        state.paths={fixture.original,fixture.directory+L"\\saved-shortcut.lnk"};
        saved.cards.push_back(state);
    }
    CheckUi(desk::testing::SaveStateToDirectory(data,saved),"save user layout outside any executable directory");
    desk::AppState restored;
    CheckUi(desk::testing::LoadStateFromDirectory(desk::testing::DataDirectoryUnder(userRoot),restored) &&
        restored.cards.size()==3,"a subsequent version reopens all saved user cards");
    for(size_t i=0;i<restored.cards.size();++i) {
        const auto& state=restored.cards[i];
        Card* card=CreateCard(state);
        CheckUi(card && card->state.paths==saved.cards[i].paths && card->state.locked==saved.cards[i].locked &&
            EqualRect(WindowRect(card->hwnd),desk::ResolveCardRect(saved.cards[i],monitors)),
            "reopening preserves file order, lock state and custom near-edge placement without snapping");
        if(card)HandleCommand(card,RemoveCard);
    }
    for(const auto* name:{L"state.dat",L"state.backup.dat",L"state.dat.tmp",L"state.backup.dat.tmp"})
        DeleteFileW((data+L"\\"+name).c_str());
    CheckUi(RemoveDirectoryW(data.c_str()) && RemoveDirectoryW(userRoot.c_str()),"remove only isolated user-data fixtures");
}

void TestReferencesAndInput(Card& card, const Fixture& fixture) {
    const std::wstring unicode = fixture.directory + L"\\\u9879\u76ee \U0001F4BB.lnk";
    AddPaths(card, {fixture.original, unicode, fixture.original, L"relative.txt", L"https://example.invalid"});
    CheckUi(card.state.paths.size() == 2 && card.items.size() == 2,
        "add missing Unicode references, ignore duplicates and unsupported paths");
    CheckUi(card.state.paths[1] == unicode && card.items[1].label == L"\u9879\u76ee \U0001F4BB",
        "Unicode shortcut label and reference remain intact");
    CheckUi(!card.status.empty(), "unsupported reference is explained inside card");
    CheckUi(fixture.originalExists(), "adding a reference preserves original file");

    std::vector<std::wstring> many;
    for (int i = 0; i < 90; ++i)
        many.push_back(fixture.directory + L"\\missing-" + std::to_wstring(i) + L".lnk");
    AddPaths(card, many);
    HandleCommand(&card, List);
    CheckUi(card.state.layout == desk::Layout::List, "switch to list view");
    CheckUi(RowHeight(card) == Dip(card, 24), "compact list uses twenty-four DIP rows");
    const RECT firstRow = ItemRect(card, 0), secondRow = ItemRect(card, 1);
    CheckUi(Height(firstRow) == Dip(card, 24) && firstRow.bottom == secondRow.top &&
        HitItem(card, POINT{firstRow.left + 1, firstRow.bottom - 1}) == 0 &&
        HitItem(card, POINT{secondRow.left + 1, secondRow.top}) == 1,
        "compact list hit testing follows adjacent row boundaries");
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_END, 0);
    RECT selected = ItemRect(card, card.selected), content = ContentRect(card);
    CheckUi(card.selected == static_cast<int>(card.items.size()) - 1 && card.scroll > 0 &&
        selected.bottom <= content.bottom, "list End key selects and reveals final item");
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_HOME, 0);
    CheckUi(card.selected == 0 && card.scroll == 0, "list Home key returns to first row");
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_DOWN, 0);
    CheckUi(card.selected == 1, "list Down key advances one item");

    HandleCommand(&card, Grid);
    CheckUi(card.state.layout == desk::Layout::Grid, "switch to grid view");
    card.selected = -1;
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_DOWN, 0);
    CheckUi(card.selected == 0, "first arrow key selects the first grid item");
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_HOME, 0);
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_DOWN, 0);
    CheckUi(card.selected == Columns(card), "grid Down key advances one visual row");
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_END, 0);
    selected = ItemRect(card, card.selected);
    content = ContentRect(card);
    CheckUi(card.selected == static_cast<int>(card.items.size()) - 1 && card.scroll > 0 &&
        selected.bottom <= content.bottom, "grid End key reveals final item");
    SendMessageW(card.hwnd, WM_KEYDOWN, VK_HOME, 0);
    card.wheelRemainder = 0;
    SendMessageW(card.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA / 2)), 0);
    CheckUi(card.scroll == 0, "partial wheel delta is accumulated");
    SendMessageW(card.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA / 2)), 0);
    CheckUi(card.scroll > 0 && card.scroll <= MaxScroll(card), "full wheel step scrolls within bounds");

    if (card.state.locked) HandleCommand(&card, Lock);
    CheckUi(HitAt(card, 1, 1) == HTTOPLEFT && HitAt(card, Dip(card, 65), Dip(card, 22)) == HTCAPTION,
        "unlocked card exposes resize corner and draggable title");
    HandleCommand(&card, Lock);
    CheckUi(card.state.locked && HitAt(card, 1, 1) == HTCLIENT &&
        HitAt(card, Dip(card, 65), Dip(card, 22)) == HTCLIENT,
        "locked card disables movement and resizing");
    HandleCommand(&card, Lock);
    CheckUi(!card.state.locked, "unlock toggle updates stored state");

    HandleCommand(&card, Rename);
    CheckUi(card.editor && IsWindow(card.editor), "rename creates native edit control");
    if (card.editor) {
        SendMessageW(card.editor, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"  \u5de5\u4f5c\u9879\u76ee \U0001F4BB  "));
        SendMessageW(card.editor, WM_KEYDOWN, VK_RETURN, 0);
        wchar_t title[256]{};
        GetWindowTextW(card.hwnd, title, ARRAYSIZE(title));
        CheckUi(!card.editor && card.state.title == L"\u5de5\u4f5c\u9879\u76ee \U0001F4BB" &&
            card.state.title == title, "Return commits trimmed Unicode title to model and HWND");
    }
    const auto committed = card.state.title;
    HandleCommand(&card, Rename);
    if (card.editor) {
        SendMessageW(card.editor, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"discard me"));
        SendMessageW(card.editor, WM_KEYDOWN, VK_ESCAPE, 0);
        CheckUi(!card.editor && card.state.title == committed, "Escape cancels rename");
    }
}

void TestCompactHeaderAndInput(Card& card) {
    RECT client{};
    GetClientRect(card.hwnd, &client);
    const RECT content = ContentRect(card);
    const RECT menu = MenuRect(card), lock = LockRect(card), collapse = CollapseRect(card);
    CheckUi(content.top == Dip(card, 38) && content.bottom == client.bottom - Dip(card, 8),
        "content uses the compact header and extends into the former footer");
    CheckUi(menu.left == client.right - Dip(card, 32) &&
        menu.right == client.right - Dip(card, 5) && menu.top == Dip(card, 3) &&
        menu.bottom == Dip(card, 29) && lock.right == menu.right - Dip(card, 28) &&
        lock.left == menu.left - Dip(card, 28) && collapse.right == lock.right - Dip(card, 28) &&
        collapse.left == lock.left - Dip(card, 28), "compact header button bounds respect DPI");
    CheckUi(TitleRect(card).right <= collapse.left - Dip(card, 3),
        "title reserves space for the restored collapse button");
    LOGFONTW titleFont{};
    CheckUi(GetObjectW(card.titleFont, sizeof(titleFont), &titleFont) != 0 &&
        titleFont.lfHeight == -Dip(card, 12), "compact header uses a twelve-DIP title font");

    const POINT lockPoint{(lock.left + lock.right) / 2, (lock.top + lock.bottom) / 2};
    const POINT menuPoint{(menu.left + menu.right) / 2, (menu.top + menu.bottom) / 2};
    const POINT collapsePoint{(collapse.left + collapse.right) / 2, (collapse.top + collapse.bottom) / 2};
    const POINT outside{content.left + 2, content.top + 2};
    CheckUi(HeaderHit(card, lockPoint) == 1 && HeaderHit(card, menuPoint) == 2 &&
        HeaderHit(card, collapsePoint) == 3 && HeaderHit(card, outside) == 0,
        "header hit testing distinguishes lock, menu, collapse, and content");
    CheckUi(HitAt(card, lockPoint.x, lockPoint.y) == HTCLIENT &&
        HitAt(card, menuPoint.x, menuPoint.y) == HTCLIENT &&
        HitAt(card, collapsePoint.x, collapsePoint.y) == HTCLIENT,
        "compact header buttons remain clickable rather than starting a window drag");
    const bool wasLocked = card.state.locked;
    card.state.locked = false;
    CheckUi(HitAt(card, Dip(card, 65), Dip(card, 16)) == HTCAPTION &&
        HitAt(card, Dip(card, 65), Dip(card, 34)) == HTCLIENT,
        "only the compact title strip starts a window drag");

    const POINT fillPoint{lock.left + Dip(card, 2), lock.top + Dip(card, 2)};
    SendMessageW(card.hwnd, WM_MOUSELEAVE, 0, 0);
    const COLORREF normalColor = PaintedPixel(card, fillPoint);
    SendMessageW(card.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(lockPoint.x, lockPoint.y));
    CheckUi(card.headerHover == 1, "moving over lock enables its hover effect");
    const COLORREF hoverColor = PaintedPixel(card, fillPoint);
    SendMessageW(card.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(lockPoint.x, lockPoint.y));
    CheckUi(card.headerPressed == 1 && GetCapture() == card.hwnd && !card.state.locked,
        "lock press captures the pointer and defers activation until release");
    const COLORREF pressedColor = PaintedPixel(card, fillPoint);
    CheckUi(normalColor != CLR_INVALID && hoverColor != CLR_INVALID && pressedColor != CLR_INVALID &&
        normalColor != hoverColor && hoverColor != pressedColor && normalColor != pressedColor,
        "production painter gives normal, hovered, and pressed buttons distinct visual treatments");
    SendMessageW(card.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(outside.x, outside.y));
    SendMessageW(card.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(outside.x, outside.y));
    CheckUi(card.headerPressed == 0 && GetCapture() != card.hwnd && !card.state.locked,
        "releasing outside a header button cancels activation and capture");

    SendMessageW(card.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(lockPoint.x, lockPoint.y));
    SendMessageW(card.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(lockPoint.x, lockPoint.y));
    CheckUi(card.state.locked && card.headerPressed == 0 && GetCapture() != card.hwnd,
        "release over the pressed lock button toggles it once");
    SendMessageW(card.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(lockPoint.x, lockPoint.y));
    CheckUi(card.state.locked, "unpaired release cannot activate the lock button");

    SendMessageW(card.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(menuPoint.x, menuPoint.y));
    SendMessageW(card.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(menuPoint.x, menuPoint.y));
    CheckUi(card.headerHover == 2 && card.headerPressed == 2 && GetCapture() == card.hwnd,
        "menu button exposes both hover and pressed feedback");
    ReleaseCapture();
    CheckUi(card.headerPressed == 0, "losing pointer capture clears the pressed effect");
    SendMessageW(card.hwnd, WM_MOUSELEAVE, 0, 0);
    CheckUi(card.headerHover == 0 && !card.tracking, "leaving the card clears header hover state");
    card.state.locked = wasLocked;

    card.scroll = 0;
    const RECT first = ItemRect(card, 0);
    const POINT itemPoint{first.left + Dip(card, 12), first.top + Dip(card, 12)};
    SendMessageW(card.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(itemPoint.x, itemPoint.y));
    CheckUi(card.mouseDownItem == 0 && card.selected == 0,
        "item press records the reference as a native drag candidate");
    if (GetSystemMetrics(SM_CXDRAG) > 1)
        SendMessageW(card.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(itemPoint.x + 1, itemPoint.y));
    CheckUi(card.mouseDownItem == 0, "pointer motion below the system threshold does not begin a drag");
    SendMessageW(card.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(itemPoint.x, itemPoint.y));
    CheckUi(card.mouseDownItem == -1 && GetCapture() != card.hwnd,
        "item click release cancels its pending drag and pointer capture");
}

std::vector<UINT> MenuCommands(HMENU menu) {
    std::vector<UINT> commands;
    for (int index = 0; index < GetMenuItemCount(menu); ++index) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU;
        CheckUi(GetMenuItemInfoW(menu, index, TRUE, &item) != FALSE,
            "inspect native menu item");
        if (item.fType & MFT_SEPARATOR) continue;
        if (item.hSubMenu) {
            const auto nested = MenuCommands(item.hSubMenu);
            commands.insert(commands.end(), nested.begin(), nested.end());
        } else commands.push_back(item.wID);
    }
    std::sort(commands.begin(), commands.end());
    return commands;
}

void TestCoreMenus(Card& card) {
    HMENU cardMenu = BuildMenu(&card, false);
    CheckUi(cardMenu != nullptr, "construct native card menu without opening a modal popup");
    if (cardMenu) {
        std::vector<UINT> expected{AddFiles, AddFolder, Rename, Grid, List, Lock, NewCard, RemoveCard};
        std::sort(expected.begin(), expected.end());
        CheckUi(MenuCommands(cardMenu) == expected,
            "card menu contains only core content, layout, locking, and card actions");
        CheckUi(((GetMenuState(cardMenu, Lock, MF_BYCOMMAND) & MF_CHECKED) != 0) == card.state.locked,
            "card menu lock check matches current state");
        CheckUi((GetMenuState(cardMenu, card.state.layout == desk::Layout::Grid ? Grid : List,
            MF_BYCOMMAND) & MF_CHECKED) != 0, "card menu checks the current layout");
        DestroyMenu(cardMenu);
    }
    card.selected = card.items.empty() ? -1 : 0;
    HMENU itemMenu = BuildMenu(&card, true);
    CheckUi(itemMenu != nullptr, "construct native reference menu");
    if (itemMenu) {
        std::vector<UINT> expected{Open, Reveal, RemoveItem};
        std::sort(expected.begin(), expected.end());
        CheckUi(MenuCommands(itemMenu) == expected,
            "reference menu contains only open, reveal, and remove-reference actions");
        DestroyMenu(itemMenu);
    }
    HMENU global = BuildMenu(nullptr, false);
    CheckUi(global != nullptr, "construct global tray settings menu");
    if (global) {
        const auto commands = MenuCommands(global);
        CheckUi(std::find(commands.begin(), commands.end(), Autostart) != commands.end() &&
            std::find(commands.begin(), commands.end(), DesktopMenu) != commands.end() &&
            std::find(commands.begin(), commands.end(), Quit) != commands.end(),
            "application settings and quit remain available in the global tray menu");
        DestroyMenu(global);
    }
}

void TestBoundedAnimation(Card& card) {
    const RECT content = ContentRect(card);
    const POINT position{content.left + Dip(card, 30), content.top + Dip(card, 30)};
    StartFeedback(card, true, position);
    CheckUi(card.feedbackActive && card.feedbackSuccess && card.feedbackStart != 0,
        "successful file interaction starts a short feedback animation");
    SendMessageW(card.hwnd, WM_TIMER, kAnimationTimer, 0);
    CheckUi(card.feedbackActive, "feedback remains active during its short animation interval");
    card.feedbackStart = GetTickCount64() - 200;
    SendMessageW(card.hwnd, WM_TIMER, kAnimationTimer, 0);
    CheckUi(!card.feedbackActive && !KillTimer(card.hwnd, kAnimationTimer),
        "completed animation clears feedback and stops its timer to avoid idle repaints");
    StartFeedback(card, false, position);
    CheckUi(card.feedbackActive && !card.feedbackSuccess,
        "cancelled or rejected file interaction can display bounded feedback");
    card.feedbackStart = GetTickCount64() - 200;
    SendMessageW(card.hwnd, WM_TIMER, kAnimationTimer, 0);
    CheckUi(!card.feedbackActive && !KillTimer(card.hwnd, kAnimationTimer),
        "restarted feedback also releases its timer after the animation");
}

void TestNativeDropIntoCard(const Fixture& fixture) {
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"drop integration fixture";
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create native file-drop card fixture");
    if (!card) return;
    ShowWindow(card->hwnd, SW_HIDE);
    IDropTarget* target = desk::testing::AcquireRegisteredFileDropTarget(card->hwnd);
    IDataObject* source = desk::testing::CreateFileReferenceDataObject(fixture.original);
    CheckUi(target && source, "card registers an OLE target that accepts native file data");
    if (target && source) {
        const RECT content = ContentRect(*card);
        POINT local{content.left + Dip(*card, 20), content.top + Dip(*card, 20)};
        POINT screen = local;
        ClientToScreen(card->hwnd, &screen);
        const POINTL position{screen.x, screen.y};
        DWORD effect = DROPEFFECT_COPY;
        CheckUi(SUCCEEDED(target->DragEnter(source, 0, position, &effect)) &&
            effect == DROPEFFECT_COPY && card->dropHovered,
            "native DragEnter highlights the target card and offers reference copying");
        target->DragLeave();
        CheckUi(!card->dropHovered, "native DragLeave immediately removes drop highlighting");
        effect = DROPEFFECT_COPY;
        target->DragEnter(source, 0, position, &effect);
        effect = DROPEFFECT_COPY;
        CheckUi(SUCCEEDED(target->Drop(source, 0, position, &effect)) &&
            effect == DROPEFFECT_COPY && card->state.paths.size() == 1 &&
            card->state.paths.front() == fixture.original && card->items.size() == 1 &&
            !card->dropHovered && card->feedbackActive && fixture.originalExists(),
            "native drop adds the reference, preserves the original, and starts success feedback");
        CheckUi(std::abs(card->dropPoint.x - local.x) <= 1 &&
            std::abs(card->dropPoint.y - local.y) <= 1,
            "native drop converts screen coordinates for animation positioning");
    }
    if (source) source->Release();
    if (target) target->Release();
    HandleCommand(card, RemoveCard);
}

void TestManagedDesktopDropIdentity(const Fixture& fixture) {
    // Inject only directories inside this process's already isolated fixture.
    // Production Desktop, configuration, and managed storage are never read.
    const auto root = fixture.directory + L"\\desktop-drop";
    const auto config = root + L"\\Config", desktop = root + L"\\Desktop";
    const auto common = root + L"\\Public", storage = root + L"\\Managed";
    bool directoriesReady = true;
    for (const auto& directory : {root, config, desktop, common, storage}) {
        const bool created = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
        CheckUi(created, "create isolated desktop identity fixture directory");
        directoriesReady = directoriesReady && created;
    }
    const auto original = desktop + L"\\native-drag.txt";
    const char payload[] = "owned desktop drag identity\n";
    HANDLE file = directoriesReady ? CreateFileW(original.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    bool fileReady = file != INVALID_HANDLE_VALUE;
    if (fileReady) {
        DWORD written = 0;
        fileReady = WriteFile(file, payload, sizeof(payload) - 1, &written, nullptr) &&
            written == sizeof(payload) - 1;
        CloseHandle(file);
    }
    CheckUi(fileReady, "create dedicated desktop drag file without touching other UI fixtures");
    const bool initialized = fileReady &&
        desk::testing::InitializeDesktopItemsAt(config, desktop, common, storage);
    CheckUi(initialized, "initialize desktop ownership using only explicitly injected temporary directories");
    std::wstring managed;
    Card* first = nullptr;
    Card* second = nullptr;
    if (initialized) {
        CheckUi(desk::ManageDesktopItem(original) == desk::DesktopManageResult::Managed,
            "acquire temporary desktop file for native CF_HDROP identity regression");
        managed = desk::ResolveDesktopItem(original);
        CheckUi(managed != original && managed.find(storage + L"\\") == 0 &&
            GetFileAttributesW(original.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(managed.c_str()) != INVALID_FILE_ATTRIBUTES,
            "temporary desktop file disappears only after entering isolated managed storage");
        desk::CardState one, two;
        one.id = app.nextId++;
        two.id = app.nextId++;
        one.title = L"managed first";
        two.title = L"managed second";
            two.layout=desk::Layout::List;
        first = CreateCard(std::move(one));
        second = CreateCard(std::move(two));
        CheckUi(first && second, "create two cards to share the managed desktop reference");
        if (first && second) {
            ShowWindow(first->hwnd, SW_HIDE);
            ShowWindow(second->hwnd, SW_HIDE);
            AddPaths(*first, {managed});
            CheckUi(first->state.paths == std::vector<std::wstring>{original} &&
                first->items.size() == 1 && ReferenceCount(original) == 1,
                "adding an actual managed Shell path stores the original persistent desktop identity");
            IDropTarget* target = desk::testing::AcquireRegisteredFileDropTarget(first->hwnd);
            IDataObject* source = desk::testing::CreateFileReferenceDataObject(managed);
            CheckUi(target && source, "create native CF_HDROP containing an already managed physical path");
            if (target && source) {
                const RECT content = ContentRect(*first);
                POINT point{content.left + Dip(*first, 20), content.top + Dip(*first, 20)};
                ClientToScreen(first->hwnd, &point);
                const POINTL position{point.x, point.y};
                DWORD effect = DROPEFFECT_COPY;
                target->DragEnter(source, 0, position, &effect);
                effect = DROPEFFECT_COPY;
                CheckUi(SUCCEEDED(target->Drop(source, 0, position, &effect)) &&
                    effect == DROPEFFECT_COPY && first->state.paths == std::vector<std::wstring>{original} &&
                    first->items.size() == 1 && first->feedbackSuccess && ReferenceCount(original) == 1,
                    "native duplicate drop canonicalizes managed path before deduplication and reports success");
            }
            if (source) source->Release();
            if (target) target->Release();
            AddPaths(*second, {managed, original});
            CheckUi(second->state.paths == std::vector<std::wstring>{original} &&
                second->items.size() == 1 && ReferenceCount(original) == 2 &&
                ReferenceCount(original, first) == 1 && ReferenceCount(original, second) == 1,
                "two cards share one original identity without duplicate references within either card");
            // Reproduce the user's legacy duplicate across cards, then exercise
            // the actual OLE target with an active application drag session.
            AddPaths(*second,{fixture.original});second->selected=1;
            if(first->items[0].icon)DestroyIcon(first->items[0].icon);
            first->items[0].icon=SolidTestIcon(RGB(241,121,37));
            auto cardDrop=[&](Card& from,Card& to,DWORD keys) {
                CardDragScope drag(from,original);
                from.draggingOut=true;from.selected=0;from.hovered=0;
                IDropTarget* drop=desk::testing::AcquireRegisteredFileDropTarget(to.hwnd);
                IDataObject* data=desk::testing::CreateFileReferenceDataObject(managed);
                DWORD effect=DROPEFFECT_COPY;POINT p{20,50};ClientToScreen(to.hwnd,&p);
                if(drop&&data){drop->DragEnter(data,keys|MK_LBUTTON,{p.x,p.y},&effect);effect=DROPEFFECT_COPY;drop->Drop(data,keys,{p.x,p.y},&effect);}
                const bool handled=app.dragHandledInside;
                if(data)data->Release();
                if(drop)drop->Release();
                if(!handled)ClearDragFeedback(from);
                return effect!=DROPEFFECT_NONE&&handled;
            };
            CheckUi(cardDrop(*first,*second,0)&&first->state.paths.empty()&&first->items.empty()&&
                second->state.paths.size()==2&&second->selected==0&&first->selected==-1&&
                !first->draggingOut&&!first->feedbackActive&&first->hovered==-1&&ReferenceCount(original)==1,
                "default card drop merges an existing target duplicate and clears source drag/selection feedback");
            CheckUi(cardDrop(*second,*first,0)&&first->state.paths==std::vector<std::wstring>{original}&&
                second->state.paths==std::vector<std::wstring>{fixture.original}&&desk::ResolveDesktopItem(original)==managed&&
                GetFileAttributesW(original.c_str())==INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(managed.c_str())!=INVALID_FILE_ATTRIBUTES,
                "card-to-card move transfers the reference and existing icon without restoring or moving the managed file");
            CheckUi(cardDrop(*first,*second,MK_CONTROL)&&first->state.paths.size()==1&&second->state.paths.size()==2&&
                ReferenceCount(original)==2&&first->items[0].icon&&second->items[1].icon&&first->items[0].icon!=second->items[1].icon,
                "Control-drop explicitly keeps a source copy with separate owned icon handles");
            ToggleCollapse(*first);
            CheckUi(cardDrop(*second,*first,0)&&!first->state.collapsed&&ReferenceCount(original)==1&&
                first->state.paths.size()==1&&second->state.paths.size()==1,
                "dropping into a collapsed card expands it and selects the sole transferred reference");
            CheckUi(cardDrop(*first,*first,0)&&first->state.paths.size()==1&&ReferenceCount(original)==1,
                "dropping onto the source card is an accepted no-op without duplication");
            desk::CardState full;full.id=app.nextId++;full.title=L"internal full target";
            for(size_t i=0;i<kMaxItems;++i)full.paths.push_back(fixture.directory+L"\\full-"+std::to_wstring(i)+L".txt");
            Card* fullCard=CreateCard(std::move(full));
            if(fullCard){ShowWindow(fullCard->hwnd,SW_HIDE);CheckUi(!cardDrop(*first,*fullCard,0)&&first->state.paths.size()==1&&
                desk::ResolveDesktopItem(original)==managed&&ReferenceCount(original)==1,
                "a full target rejects a move while preserving source and sole managed payload");HandleCommand(fullCard,RemoveCard);}
        }
        CheckUi(desk::RestoreDesktopItems() &&
            GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES &&
            desk::ResolveDesktopItem(original) == original,
            "restore the isolated managed desktop file before continuing unrelated UI tests");
        HANDLE restored = CreateFileW(original.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        char bytes[sizeof(payload)]{};
        DWORD read = 0;
        const bool preserved = restored != INVALID_HANDLE_VALUE &&
            ReadFile(restored, bytes, sizeof(payload) - 1, &read, nullptr) &&
            read == sizeof(payload) - 1 && std::memcmp(bytes, payload, sizeof(payload) - 1) == 0;
        if (restored != INVALID_HANDLE_VALUE) CloseHandle(restored);
        CheckUi(preserved, "native managed drag identity and restoration preserve file contents exactly");
    }
    if (first) HandleCommand(first, RemoveCard);
    if (second) HandleCommand(second, RemoveCard);
    desk::ClearDesktopItems();
    // Remove only known files and now-empty directories under the explicit
    // fixture. Avoid a recursive deletion of any derived storage path.
    DeleteFileW(original.c_str());
    for (const auto* name : {L"desktop-items.dat", L"desktop-items.backup.dat",
            L"desktop-items.dat.tmp", L"desktop-items.backup.dat.tmp"})
        DeleteFileW((config + L"\\" + name).c_str());
    if (managed.find(storage + L"\\") == 0) {
        const auto parent = managed.substr(0, managed.find_last_of(L'\\'));
        RemoveDirectoryW(parent.c_str());
    }
    for (const auto& directory : {storage, common, desktop, config, root})
        CheckUi(RemoveDirectoryW(directory.c_str()) != FALSE,
            "remove restored desktop identity fixture without leaving managed data or journals");
}

void TestHiddenExternalCopy(const Fixture& fixture) {
    const auto root=fixture.directory+L"\\external-visibility",data=root+L"\\Config",desktop=root+L"\\Desktop",outside=root+L"\\copy.txt";
    for(const auto& dir:{root,data,desktop})CheckUi(CreateDirectoryW(dir.c_str(),nullptr)!=FALSE,"create isolated external-copy directory");
    const auto source=desktop+L"\\source.txt";
    CheckUi(CopyFileW(fixture.original.c_str(),source.c_str(),TRUE)!=FALSE,"create synthetic desktop source for external-copy test");
    CheckUi(desk::testing::InitializeHiddenItemsAt(data,desktop,L"")&&desk::HideDesktopItem(source)==desk::DesktopManageResult::Managed,
        "hide external-copy source in place");
    {
        ExternalVisibilityScope visibility{source};
        CheckUi(ExternalVisibilityScope::Prepare(&visibility)&&!(GetFileAttributesW(source.c_str())&desk::kVisibilityBits),
            "external-drop preparation restores source attributes before recipient copies");
        CheckUi(CopyFileW(source.c_str(),outside.c_str(),TRUE)&&!(GetFileAttributesW(outside.c_str())&desk::kVisibilityBits),
            "external copy does not inherit organizer visibility bits");
    }
    CheckUi(desk::IsHiddenDesktopItem(source)&&desk::ReleaseHiddenDesktopItem(source)&&!(GetFileAttributesW(source.c_str())&desk::kVisibilityBits),
        "modal drag scope rehides original and normal release restores it");
    desk::ClearHiddenItems();DeleteFileW(source.c_str());DeleteFileW(outside.c_str());
    for(const auto* name:{L"hidden-items.dat",L"hidden-items.backup.dat"})DeleteFileW((data+L"\\"+name).c_str());
    for(const auto& dir:{desktop,data,root})CheckUi(RemoveDirectoryW(dir.c_str())!=FALSE,"remove empty external-copy fixture directories");
}

void TestSameCardReordering(const Fixture& fixture) {
    for(const auto layout:{desk::Layout::List,desk::Layout::Grid}) {
        desk::CardState state;state.id=app.nextId++;state.title=L"reorder";state.layout=layout;
        for(int i=0;i<7;++i)state.paths.push_back(fixture.directory+L"\\order-"+std::to_wstring(i)+L".txt");
        Card* card=CreateCard(std::move(state));if(!card){CheckUi(false,"create reorder card");continue;}
        ShowWindow(card->hwnd,SW_HIDE);const auto original=card->state.paths;
        const uint64_t token=card->items[0].token;card->items[0].icon=TestIcon();const HICON icon=card->items[0].icon;
        auto drop=[&](int from,POINT point,DWORD keys=0) {
            const std::wstring reference=card->state.paths[from];CardDragScope drag(*card,reference);
            IDataObject* data=desk::testing::CreateFileReferenceDataObject(reference);
            IDropTarget* target=desk::testing::AcquireRegisteredFileDropTarget(card->hwnd);
            ClientToScreen(card->hwnd,&point);DWORD effect=DROPEFFECT_COPY;bool ok=false;
            if(data&&target){target->DragEnter(data,keys|MK_LBUTTON,{point.x,point.y},&effect);effect=DROPEFFECT_COPY;
                target->Drop(data,keys,{point.x,point.y},&effect);ok=app.dragHandledInside&&effect!=DROPEFFECT_NONE;}
            if(data)data->Release();
            if(target)target->Release();
            return ok;
        };
        const RECT last=ItemRect(*card,6);
        POINT end=layout==desk::Layout::List?POINT{last.left+4,last.bottom-2}:POINT{last.right-2,last.top+5};
        CheckUi(drop(0,end)&&card->state.paths.back()==original[0]&&card->selected==6&&
            card->items.back().token==token&&card->items.back().icon==icon&&card->state.paths.size()==7,
            "native same-card drop moves first to last in list/grid while retaining icon ownership and identity");
        const RECT first=ItemRect(*card,0);const POINT beginning{first.left+1,first.top+1};
        CheckUi(drop(6,beginning,MK_CONTROL)&&card->state.paths==original&&card->selected==0&&card->items[0].icon==icon,
            "same-card Control-drop reorders without duplicate entries");
        const RECT middle=ItemRect(*card,3);const POINT before{middle.left+1,middle.top+1};
        CheckUi(drop(0,before)&&card->selected==2&&card->state.paths[2]==original[0]&&card->state.paths[3]==original[3],
            "forward insertion compensates for removing the source entry");
        const auto unchanged=card->state.paths;
        RECT self=ItemRect(*card,2);CheckUi(drop(2,{self.left+1,self.top+1})&&card->state.paths==unchanged,
            "dropping into own insertion gap leaves order unchanged");
        card->scroll=RowHeight(*card);const RECT scrolled=ItemRect(*card,layout==desk::Layout::List?1:Columns(*card));
        CheckUi(InsertionIndex(*card,{scrolled.left+1,scrolled.top+1})==(layout==desk::Layout::List?1:Columns(*card)),
            "insertion hit testing includes scroll offset in both layouts");
        desk::AppState saved,loaded;saved.nextId=app.nextId;saved.cards.push_back(card->state);
        const auto directory=fixture.directory+L"\\reorder-layout";
        CheckUi(desk::testing::SaveStateToDirectory(directory,saved)&&desk::testing::LoadStateFromDirectory(directory,loaded)&&
            loaded.cards.size()==1&&loaded.cards[0].paths==card->state.paths,"reordered paths survive layout save/reload");
        for(const auto* name:{L"state.dat",L"state.backup.dat"})DeleteFileW((directory+L"\\"+name).c_str());
        RemoveDirectoryW(directory.c_str());
        HandleCommand(card,RemoveCard);
    }
}

void TestDeleteWhilePressed(const Fixture& fixture) {
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"pressed delete fixture";
    state.layout = desk::Layout::List;
    state.paths = {fixture.directory + L"\\other-reference.txt", fixture.original};
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create last-item deletion fixture");
    if (!card) return;
    ShowWindow(card->hwnd, SW_HIDE);
    const RECT last = ItemRect(*card, 1);
    const POINT pressed{last.left + Dip(*card, 12), last.top + Dip(*card, 12)};
    SendMessageW(card->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pressed.x, pressed.y));
    CheckUi(card->mouseDownItem == 1 && card->selected == 1 && GetCapture() == card->hwnd,
        "last-item press starts a captured drag candidate");
    SendMessageW(card->hwnd, WM_KEYDOWN, VK_DELETE, 0);
    CheckUi(card->items.size() == 1 && card->state.paths.size() == 1 &&
        card->mouseDownItem == -1 && GetCapture() != card->hwnd && fixture.originalExists(),
        "Delete while pressed cancels the drag candidate before erasing its reference");
    SendMessageW(card->hwnd, WM_MOUSEMOVE, MK_LBUTTON,
        MAKELPARAM(pressed.x + GetSystemMetrics(SM_CXDRAG) + 10, pressed.y));
    CheckUi(card->mouseDownItem == -1 && !card->draggingOut && app.modalDepth == 0 &&
        card->items.size() == 1 && card->state.paths.size() == 1,
        "motion after deleting the pressed last item cannot enter a native drag loop");
    HandleCommand(card, RemoveCard);
}

void TestFullCardRejectsNewDrop(const Fixture& fixture) {
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"full card drop fixture";
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create full-card drop fixture");
    if (!card) return;
    ShowWindow(card->hwnd, SW_HIDE);
    // Fill the model directly: this test needs capacity handling, not icon jobs
    // or filesystem access for two thousand synthetic references.
    CheckUi(app.worker == nullptr, "full-card fixture runs with the icon worker stopped");
    card->state.paths.reserve(kMaxItems);
    card->items.resize(kMaxItems);
    for (size_t index = 0; index < kMaxItems; ++index) {
        card->state.paths.push_back(fixture.directory + L"\\capacity-" +
            std::to_wstring(index) + L".txt");
        card->items[index].label = L"capacity reference";
    }
    IDropTarget* target = desk::testing::AcquireRegisteredFileDropTarget(card->hwnd);
    IDataObject* source = desk::testing::CreateFileReferenceDataObject(fixture.original);
    CheckUi(target && source, "full card retains its native file-drop target");
    if (target && source) {
        const RECT content = ContentRect(*card);
        POINT screen{content.left + Dip(*card, 20), content.top + Dip(*card, 20)};
        ClientToScreen(card->hwnd, &screen);
        const POINTL position{screen.x, screen.y};
        DWORD effect = DROPEFFECT_COPY;
        target->DragEnter(source, 0, position, &effect);
        effect = DROPEFFECT_COPY;
        CheckUi(SUCCEEDED(target->Drop(source, 0, position, &effect)) &&
            effect == DROPEFFECT_NONE && card->state.paths.size() == kMaxItems &&
            card->items.size() == kMaxItems && !card->dropHovered &&
            card->feedbackActive && !card->feedbackSuccess && fixture.originalExists(),
            "dropping a new file into a full card reports rejection and preserves its original");
        CheckUi(std::find(card->state.paths.begin(), card->state.paths.end(), fixture.original) ==
            card->state.paths.end(), "rejected capacity drop does not add a hidden reference");

        IDataObject* duplicate = desk::testing::CreateFileReferenceDataObject(card->state.paths.front());
        CheckUi(duplicate != nullptr, "create an already-present reference drop");
        if (duplicate) {
            effect = DROPEFFECT_COPY;
            target->DragEnter(duplicate, 0, position, &effect);
            effect = DROPEFFECT_COPY;
            CheckUi(SUCCEEDED(target->Drop(duplicate, 0, position, &effect)) &&
                effect == DROPEFFECT_COPY && card->state.paths.size() == kMaxItems &&
                card->items.size() == kMaxItems && card->feedbackSuccess,
                "an already-present reference remains a successful no-op at capacity");
            duplicate->Release();
        }
    }
    if (source) source->Release();
    if (target) target->Release();
    HandleCommand(card, RemoveCard);
}

void TestDpiAndDisplay(Card& card) {
    const UINT dpi = WindowDpi(card.hwnd);
    RECT desired = WindowRect(card.hwnd);
    OffsetRect(&desired, 2, 3);
    SendMessageW(card.hwnd, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&desired));
    CheckUi(card.dpi == dpi && EqualRect(WindowRect(card.hwnd), desired),
        "same-DPI message applies suggested rectangle and updates UI scale");

    RECT ignored = desired;
    OffsetRect(&ignored, 25, 25);
    card.placing = true;
    SendMessageW(card.hwnd, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&ignored));
    card.placing = false;
    CheckUi(EqualRect(WindowRect(card.hwnd), desired), "programmatic placement owns its target rectangle");
    app.displayChanging = true;
    SendMessageW(card.hwnd, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&ignored));
    CheckUi(EqualRect(WindowRect(card.hwnd), desired), "pending display reflow suppresses suggested rectangle");
    app.displayChanging = false;

    desk::CaptureCardRect(card.state, card.hwnd);
    const RECT roundTrip = desk::ResolveCardRect(card.state, desk::EnumerateMonitors());
    const RECT captured = WindowRect(card.hwnd);
    CheckUi(std::abs(roundTrip.left - captured.left) <= 1 && std::abs(roundTrip.top - captured.top) <= 1 &&
        std::abs(roundTrip.right - captured.right) <= 1 && std::abs(roundTrip.bottom - captured.bottom) <= 1,
        "native capture and DIP resolution round trip within one physical pixel");

    SetTimer(app.controller, kSaveTimer, 30000, nullptr);
    SendMessageW(app.controller, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    CheckUi(app.displayChanging && !KillTimer(app.controller, kSaveTimer),
        "display change cancels pending save before delayed reflow");
    const int preservedX = card.state.x;
    // Disable the preview branch only while displayChanging is true. Its guard
    // must return before any production directory is read or written.
    struct RestorePreview { ~RestorePreview() { app.preview = true; } } restore;
    app.preview = false;
    Dirty();
    CheckUi(!KillTimer(app.controller, kSaveTimer), "pending reflow prevents Dirty from starting a save timer");
    SendMessageW(app.controller, WM_TIMER, kSaveTimer, 0);
    CheckUi(card.state.x == preservedX && app.displayChanging, "pending save cannot capture transitional geometry");
    app.preview = true;

    card.state.monitor = L"MintDesk-disconnected-test-monitor";
    card.state.x = 90000;
    card.state.y = 90000;
    SendMessageW(app.controller, WM_TIMER, kDisplayTimer, 0);
    RECT current = WindowRect(card.hwnd);
    const auto monitors = desk::EnumerateMonitors();
    CheckUi(!app.displayChanging && !monitors.empty() && card.state.monitor == monitors.front().device &&
        current.left >= monitors.front().work.left && current.top >= monitors.front().work.top &&
        current.right <= monitors.front().work.right && current.bottom <= monitors.front().work.bottom,
        "display reflow recovers disconnected monitor and persists reachable geometry");
    SendMessageW(app.controller, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
    CheckUi(app.displayChanging, "work-area change schedules display reflow");
    SendMessageW(app.controller, WM_TIMER, kDisplayTimer, 0);
    CheckUi(!app.displayChanging, "work-area reflow completes");
}

void PositionForTest(Card& card, const RECT& rect) {
    card.placing = true;
    CheckUi(SetWindowPos(card.hwnd, nullptr, rect.left, rect.top, Width(rect), Height(rect),
        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "position isolated geometry fixture");
    card.placing = false;
}

bool AboveWindow(HWND higher, HWND lower) {
    for (HWND cursor = higher; cursor; cursor = GetWindow(cursor, GW_HWNDNEXT))
        if (cursor == lower) return true;
    return false;
}

void CheckDesktopBand(Card& card, HWND ordinary, HWND desktop, const char* message) {
    const HWND backdrop = card.glass.Window();
    const bool valid = AboveWindow(ordinary, card.hwnd) && AboveWindow(card.hwnd, desktop) &&
        !(GetWindowLongPtrW(card.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
        (!backdrop || !IsWindowVisible(backdrop) ||
            (AboveWindow(card.hwnd, backdrop) && AboveWindow(backdrop, desktop)));
    CheckUi(valid, message);
    if (!valid) {
        std::fprintf(stderr, "Layer detail: ordinaryAbove=%d cardAboveDesktop=%d cardTopmost=%d backdropVisible=%d cardAboveBackdrop=%d backdropAboveDesktop=%d\n",
            AboveWindow(ordinary, card.hwnd), AboveWindow(card.hwnd, desktop),
            (GetWindowLongPtrW(card.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
            backdrop && IsWindowVisible(backdrop), backdrop && AboveWindow(card.hwnd, backdrop),
            backdrop && AboveWindow(backdrop, desktop));
    }
}

void CALLBACK DismissLayerTestMenu(HWND window, UINT, UINT_PTR timer, DWORD) {
    KillTimer(window, timer);
    EndMenu();
}

void TestDesktopLayerAndKeyboard(const Fixture& fixture) {
    const HWND desktop = DesktopIconHost();
    CheckUi(desktop && FindWindowExW(desktop, nullptr, L"SHELLDLL_DefView", nullptr),
        "desktop band is anchored to Explorer's actual icon host rather than a wallpaper WorkerW");
    if (!desktop) return;
    constexpr wchar_t ordinaryClass[] = L"MintDesk.TestOrdinary.1";
    WNDCLASSEXW info{};
    info.cbSize = sizeof(info);
    info.lpfnWndProc = DefWindowProcW;
    info.hInstance = app.instance;
    info.lpszClassName = ordinaryClass;
    CheckUi(RegisterClassExW(&info) != 0, "register isolated ordinary application window class");
    const int outsideX = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 4096;
    const int outsideY = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) + 4096;
    HWND ordinary = CreateWindowExW(WS_EX_TOOLWINDOW, ordinaryClass, L"MintDesk isolated ordinary layer test",
        WS_OVERLAPPEDWINDOW, outsideX, outsideY, 300, 230, nullptr, nullptr, app.instance, nullptr);
    CheckUi(ordinary != nullptr, "create a real ordinary HWND without accessing application data");
    if (!ordinary) { UnregisterClassW(ordinaryClass, app.instance); return; }
    ShowWindow(ordinary, SW_SHOWNOACTIVATE);
    SetWindowPos(ordinary, HWND_TOP, outsideX, outsideY, 300, 230, SWP_NOACTIVATE);
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"desktop layer fixture";
    state.layout = desk::Layout::List;
    state.paths = {fixture.original, fixture.directory + L"\\layer-second.txt"};
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create isolated interactive card layer fixture");
    if (card) {
        RECT rect{outsideX + 30, outsideY + 30, outsideX + 370, outsideY + 350};
        PositionForTest(*card, rect);
        ShowWindow(card->hwnd, SW_SHOWNOACTIVATE);
        PlaceAtDesktop(card->hwnd);
        CheckDesktopBand(*card, ordinary, desktop,
            "newly shown card and its glass remain above desktop icons and below an ordinary application");
        CheckUi((GetWindowLongPtrW(card->hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) &&
            SendMessageW(card->hwnd, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(card->hwnd),
                MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
            "ordinary mouse activation is suppressed before explicit card input requests focus");
        SetActiveWindow(ordinary);
        const RECT first = ItemRect(*card, 0);
        const POINT item{first.left + Dip(*card, 10), (first.top + first.bottom) / 2};
        SendMessageW(card->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(item.x, item.y));
        CheckUi(card->selected == 0 && GetCapture() == card->hwnd &&
            GetFocus() == card->hwnd && GetActiveWindow() == card->hwnd,
            "clicking an item preserves pointer capture and actual keyboard focus for card navigation");
        SendMessageW(card->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(item.x, item.y));
        SendMessageW(GetFocus(), WM_KEYDOWN, VK_DOWN, 0);
        CheckUi(card->selected == 1, "keyboard navigation reaches the card through its real focused HWND");
        CheckDesktopBand(*card, ordinary, desktop,
            "an item click and keyboard focus cannot lift a card above an ordinary application");

        SetForegroundWindow(card->hwnd);
        CheckDesktopBand(*card, ordinary, desktop,
            "SetForegroundWindow alone cannot escape the desktop band");
        SetActiveWindow(card->hwnd);
        CheckDesktopBand(*card, ordinary, desktop,
            "SetActiveWindow alone cannot escape the desktop band");
        BringWindowToTop(card->hwnd);
        CheckDesktopBand(*card, ordinary, desktop,
            "explicit activation and BringWindowToTop cannot escape the desktop band");
        SetWindowPos(card->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        CheckDesktopBand(*card, ordinary, desktop,
            "a topmost promotion request is constrained to the normal desktop band");

        constexpr UINT_PTR cancelMenu = 0x7f01;
        CheckUi(SetTimer(card->hwnd, cancelMenu, 40, DismissLayerTestMenu) != 0,
            "arm bounded cancellation for a real native context-menu interaction");
        Menu(card, POINT{outsideX, outsideY});
        KillTimer(card->hwnd, cancelMenu);
        CheckUi(app.modalDepth == 0, "native context menu returns without retaining a modal scope");
        CheckDesktopBand(*card, ordinary, desktop,
            "opening and closing the native card menu leaves the card below ordinary windows");

        BeginRename(*card);
        CheckUi(card->editor && GetFocus() == card->editor && GetActiveWindow() == card->hwnd,
            "explicit rename retains native text-input keyboard focus in the desktop band");
        CheckDesktopBand(*card, ordinary, desktop,
            "a focused native title editor cannot place its card above ordinary windows");
        LowerCards();
        CheckUi(card->editor && GetFocus() == card->editor,
            "lowering an actively edited card preserves its native input focus");
        CheckDesktopBand(*card, ordinary, desktop,
            "desktop lowering includes cards with an active title editor");
        FinishRename(*card, false);

        const RECT beforeResize = WindowRect(card->hwnd);
        SetWindowPos(card->hwnd, HWND_TOPMOST, beforeResize.left, beforeResize.top,
            Width(beforeResize) + Dip(*card, 17), Height(beforeResize) + Dip(*card, 13), SWP_NOACTIVATE);
        const RECT resized = WindowRect(card->hwnd);
        CheckUi(Width(resized) > Width(beforeResize) && Height(resized) > Height(beforeResize),
            "z-order enforcement preserves native geometry changes during resize");
        CheckDesktopBand(*card, ordinary, desktop,
            "resizing while requesting topmost still cannot cover an ordinary application");
        HandleCommand(nullptr, ShowCards);
        CheckDesktopBand(*card, ordinary, desktop,
            "the desktop Open/ShowCards command preserves the desktop band");
        for (auto& existing : app.cards) ShowWindow(existing->hwnd, SW_HIDE);
        HandleCommand(card, RemoveCard);
    }
    DestroyWindow(ordinary);
    CheckUi(UnregisterClassW(ordinaryClass, app.instance) != FALSE,
        "destroy isolated ordinary application window and unregister its class");
}

void TestGlassBackdropLifecycle() {
    const BYTE previousOpacity = app.opacity;
    app.opacity = 0;
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"glass lifecycle fixture";
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create native blur lifecycle fixture");
    if (!card) { app.opacity = previousOpacity; return; }
    const HWND backdrop = card->glass.Window();
    CheckUi(backdrop && IsWindow(backdrop), "each card owns a bounded native backdrop resource");
    if (backdrop) {
        CheckUi((GetWindowLongPtrW(backdrop, GWL_STYLE) & WS_DISABLED) &&
            (GetWindowLongPtrW(backdrop, GWL_EXSTYLE) & WS_EX_NOACTIVATE) &&
            SendMessageW(backdrop, WM_NCHITTEST, 0, 0) == HTTRANSPARENT,
            "blur backdrop cannot take keyboard focus or intercept card pointer input");
        ShowWindow(card->hwnd, SW_HIDE);
        CheckUi(!IsWindowVisible(backdrop), "hiding a card also hides its compositor backdrop");
        ShowWindow(card->hwnd, SW_SHOWNOACTIVATE);
        SyncBackdrop(*card);
        const bool accepted = card->glass.BlurAccepted();
        CheckUi((IsWindowVisible(backdrop) != FALSE) == accepted,
            "supported system blur is shown while a rejected effect uses the translucent foreground fallback");
        RECT rect = WindowRect(card->hwnd);
        OffsetRect(&rect, Dip(*card, 11), Dip(*card, 13));
        rect.right += Dip(*card, 19);
        rect.bottom += Dip(*card, 17);
        PositionForTest(*card, rect);
        if (accepted) CheckUi(EqualRect(WindowRect(backdrop), WindowRect(card->hwnd)) &&
            AboveWindow(card->hwnd, backdrop), "visible glass geometry and z-order follow native card movement and resizing");
        ToggleCollapse(*card);
        if (accepted) CheckUi(EqualRect(WindowRect(backdrop), WindowRect(card->hwnd)) &&
            Height(WindowRect(backdrop)) == Dip(*card, 32), "collapsed card keeps its glass within the compact header bounds");
        ToggleCollapse(*card);
        if (accepted) CheckUi(EqualRect(WindowRect(backdrop), WindowRect(card->hwnd)),
            "expansion restores matching native foreground and blur bounds");
        app.opacity = 255;
        SetOpacity();
        CheckUi(!IsWindowVisible(backdrop), "fully opaque background disables and hides the blur resource");
        app.opacity = 0;
        SetOpacity();
        CheckUi((IsWindowVisible(backdrop) != FALSE) == card->glass.BlurAccepted(),
            "restoring default glass reapplies supported blur without requiring it on restricted systems");
        ShowWindow(card->hwnd, SW_HIDE);
        CheckUi(!IsWindowVisible(backdrop), "blur is hidden immediately when the foreground card is hidden");
    }
    HandleCommand(card, RemoveCard);
    CheckUi(!IsWindow(backdrop), "removing a card destroys its native blur window");
    app.opacity = previousOpacity;
    SetOpacity();
}

void TestBackgroundOnlyTransparency(const Fixture& fixture) {
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"MMMMMM";
    state.layout = desk::Layout::List;
    state.paths = {fixture.original};
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create background-only transparency fixture");
    if (!card) return;
    ShowWindow(card->hwnd, SW_HIDE);
    const BYTE originalOpacity = app.opacity;
    const COLORREF iconColor = RGB(241, 121, 37);
    if (!card->items.empty()) {
        if (card->items[0].icon) DestroyIcon(card->items[0].icon);
        card->items[0].icon = SolidTestIcon(iconColor);
        card->items[0].label = L"MMMMMM";
    }
    card->feedbackActive = false;
    card->status.clear();
    card->selected = card->hovered = -1;
    const RECT row = ItemRect(*card, 0);
    const POINT iconCenter{row.left + Dip(*card, 15), (row.top + row.bottom) / 2};
    const RECT title = TitleRect(*card);
    std::vector<uint32_t> transparent;
    int transparentWidth = 0, transparentHeight = 0;
    size_t titleCorePixels = 0, labelCorePixels = 0;
    for (BYTE opacity : {BYTE{0}, BYTE{179}, BYTE{255}}) {
        app.opacity = opacity;
        int width = 0, height = 0;
        const auto pixels = SurfacePixels(*card, width, height);
        if (pixels.empty()) continue;
        const POINT background{width - Dip(*card, 20), height - Dip(*card, 20)};
        const uint32_t backgroundPixel = pixels[static_cast<size_t>(background.y) * width + background.x];
        CheckUi((backgroundPixel >> 24) == (opacity ? opacity : BYTE{96}),
            "only empty background follows opacity, with the default using a light translucent glass tint");
        CheckUi(pixels[static_cast<size_t>(iconCenter.y) * width + iconCenter.x] == OpaquePixel(iconColor),
            "native icon retains its exact RGB and full alpha at every background opacity");
        if (opacity == 0) {
            transparent = pixels;
            transparentWidth = width;
            transparentHeight = height;
            for (int y = title.top; y < title.bottom; ++y)
                for (int x = title.left; x < title.right; ++x)
                    if (pixels[static_cast<size_t>(y) * width + x] == OpaquePixel(kText)) ++titleCorePixels;
            for (int y = row.top; y < row.bottom; ++y)
                for (int x = row.left + Dip(*card, 34); x < row.right; ++x)
                    if (pixels[static_cast<size_t>(y) * width + x] == OpaquePixel(kText)) ++labelCorePixels;
        } else if (!transparent.empty()) {
            CheckUi(width == transparentWidth && height == transparentHeight,
                "background opacity never changes native card dimensions");
            bool exactForeground = pixels.size() == transparent.size();
            if (exactForeground) {
                for (size_t index = 0; index < pixels.size(); ++index) {
                    if ((transparent[index] >> 24) == 255 && pixels[index] != transparent[index]) {
                        exactForeground = false;
                        break;
                    }
                }
            }
            CheckUi(exactForeground,
                "fully covered title, label, icon, and controls keep their original colors without a window-wide mask");
        }
    }
    CheckUi(titleCorePixels > 0 && labelCorePixels > 0,
        "transparency regression checks opaque glyph cores in both title and item text");
    app.opacity = originalOpacity;
    HandleCommand(card, RemoveCard);
}

void TestCollapseAndRestore(const Fixture& fixture) {
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"collapse fixture";
    state.height = 320;
    state.layout = desk::Layout::List;
    state.paths = {fixture.original};
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create collapse and expansion fixture");
    if (!card) return;
    ShowWindow(card->hwnd, SW_HIDE);
    if (!card->items.empty()) {
        if (card->items[0].icon) DestroyIcon(card->items[0].icon);
        card->items[0].icon = SolidTestIcon(RGB(241, 121, 37));
    }
    const RECT expanded = WindowRect(card->hwnd);
    desk::CaptureCardRect(card->state, card->hwnd);
    const int expandedHeight = card->state.height;
    const auto references = card->state.paths;
    const RECT collapse = CollapseRect(*card);
    const POINT center{(collapse.left + collapse.right) / 2, (collapse.top + collapse.bottom) / 2};
    const POINT fill{collapse.left + Dip(*card, 2), collapse.top + Dip(*card, 2)};
    SendMessageW(card->hwnd, WM_MOUSELEAVE, 0, 0);
    const COLORREF normal = PaintedPixel(*card, fill);
    SendMessageW(card->hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(center.x, center.y));
    const COLORREF hovered = PaintedPixel(*card, fill);
    CheckUi(card->headerHover == 3, "collapse button provides native hover feedback");
    SendMessageW(card->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(center.x, center.y));
    const COLORREF pressed = PaintedPixel(*card, fill);
    CheckUi(card->headerPressed == 3 && !card->state.collapsed && GetCapture() == card->hwnd &&
        normal != hovered && hovered != pressed && normal != pressed,
        "collapse button has a distinct pressed state and activates only on release");
    SendMessageW(card->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(center.x, center.y));
    const RECT collapsed = WindowRect(card->hwnd);
    CheckUi(card->state.collapsed && Height(collapsed) == Dip(*card, 32) &&
        card->state.height == expandedHeight && card->state.paths == references && card->items.size() == references.size(),
        "collapse reduces HWND to its header while preserving expanded height and all references");
    CheckUi(card->headerPressed == 0 && GetCapture() != card->hwnd,
        "collapse release clears pointer capture and pressed feedback");
    MINMAXINFO limits{};
    SendMessageW(card->hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    CheckUi(limits.ptMinTrackSize.y == Dip(*card, 32),
        "collapsed minimum height is exactly the compact header height");
    CheckUi(HitAt(*card, 1, Dip(*card, 16)) == HTLEFT &&
        HitAt(*card, Width(collapsed) - 1, Dip(*card, 16)) == HTRIGHT,
        "collapsed cards preserve horizontal resize handles");
    const LRESULT upper = HitAt(*card, Dip(*card, 65), 1);
    const LRESULT lower = HitAt(*card, Dip(*card, 65), Dip(*card, 31));
    CheckUi(upper != HTTOP && upper != HTTOPLEFT && upper != HTTOPRIGHT &&
        lower != HTBOTTOM && lower != HTBOTTOMLEFT && lower != HTBOTTOMRIGHT,
        "collapsed cards do not expose vertical resize handles");
    int width = 0, height = 0;
    const auto pixels = SurfacePixels(*card, width, height);
    CheckUi(height == Dip(*card, 32) &&
        std::find(pixels.begin(), pixels.end(), OpaquePixel(RGB(241, 121, 37))) == pixels.end(),
        "collapsed compositor omits the body and its opaque item icon");
    RECT moved = collapsed;
    OffsetRect(&moved, Dip(*card, 17), Dip(*card, 19));
    moved.right += Dip(*card, 20);
    PositionForTest(*card, moved);
    desk::CaptureCardRect(card->state, card->hwnd);
    CheckUi(card->state.collapsed && card->state.height == expandedHeight,
        "moving and horizontally resizing a collapsed card preserves its expanded height");
    const RECT expandButton = CollapseRect(*card);
    const POINT expandPoint{(expandButton.left + expandButton.right) / 2,
        (expandButton.top + expandButton.bottom) / 2};
    SendMessageW(card->hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(expandPoint.x, expandPoint.y));
    SendMessageW(card->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(expandPoint.x, expandPoint.y));
    SendMessageW(card->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(expandPoint.x, expandPoint.y));
    const RECT restored = WindowRect(card->hwnd);
    CheckUi(!card->state.collapsed && Height(restored) == Height(expanded) &&
        restored.left == moved.left && restored.top == moved.top && Width(restored) == Width(moved) &&
        card->state.paths == references && fixture.originalExists(),
        "expand restores the saved body height at the new position and width without touching originals");
    HandleCommand(card, RemoveCard);
}

void TestStableTitleEditing() {
    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"MMMMMM";
    Card* card = CreateCard(std::move(state));
    CheckUi(card != nullptr, "create title baseline regression fixture");
    if (!card) return;
    ShowWindow(card->hwnd, SW_HIDE);
    for (UINT dpi : {96u, 144u}) {
        card->dpi = dpi;
        Fonts(*card);
        RECT bounds = WindowRect(card->hwnd);
        bounds.right = bounds.left + Dip(*card, 400);
        bounds.bottom = bounds.top + Dip(*card, 250);
        PositionForTest(*card, bounds);
        const RECT title = TitleTextRect(*card);
        const auto before = SurfaceRegion(*card, TitleRect(*card));
        BeginRename(*card);
        CheckUi(card->editor && IsWindow(card->editor), "title editing opens native Unicode input");
        if (!card->editor) continue;
        SendMessageW(card->editor, EM_SETSEL, 6, 6);
        card->caretVisible = false;
        const auto editing = SurfaceRegion(*card, TitleRect(*card));
        CheckUi(!before.empty() && before == editing,
            "entering title editing preserves every title pixel and baseline at 100% and 150% scale");
        RECT editor = WindowRect(card->editor);
        MapWindowPoints(nullptr, card->hwnd, reinterpret_cast<POINT*>(&editor), 2);
        RECT formatting{};
        SendMessageW(card->editor, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&formatting));
        CheckUi(editor.left == title.left && editor.top == title.top &&
            std::abs(editor.left + formatting.left - title.left) <= 1 &&
            std::abs(editor.top + formatting.top - title.top) <= 1,
            "native input and shared title renderer use the same text origin");
        const auto committed = card->state.title;
        SetWindowTextW(card->editor, L"WWWW");
        SendMessageW(card->editor, EM_SETSEL, 4, 4);
        SendMessageW(card->hwnd, WM_COMMAND, MAKEWPARAM(0, EN_CHANGE), reinterpret_cast<LPARAM>(card->editor));
        card->caretVisible = false;
        CheckUi(SurfaceRegion(*card, TitleRect(*card)) != editing && card->state.title == committed,
            "EN_CHANGE renders current native edit text before committing the card title");
        FinishRename(*card, false);
        CheckUi(SurfaceRegion(*card, TitleRect(*card)) == before,
            "cancelling title editing restores the same glyph positions and title text");
    }
    HandleCommand(card, RemoveCard);
}

void TestNativeSnapping(Card& card) {
    const auto monitors = desk::EnumerateMonitors();
    CheckUi(!monitors.empty(), "snapping has a native monitor work area");
    if (monitors.empty()) return;
    const auto& monitor = monitors.front();
    const RECT work = monitor.work;
    std::vector<RECT> originalRects;
    for (const auto& existing : app.cards) originalRects.push_back(WindowRect(existing->hwnd));
    // Keep default cards out of candidate range while exercising the real HWND
    // message route. Preview mode never saves this temporary arrangement.
    for (size_t i = 1; i < app.cards.size(); ++i) {
        RECT away = originalRects[i];
        OffsetRect(&away, work.right + 4000 + static_cast<int>(i) * 1000 - away.left,
            work.bottom + 4000 - away.top);
        PositionForTest(*app.cards[i], away);
    }
    const bool wasLocked = card.state.locked;
    card.state.locked = false;
    const int width = MulDiv(228, monitor.dpi, 96);
    const int height = MulDiv(190, monitor.dpi, 96);
    RECT initial{work.left + 45, work.top + 45, work.left + 45 + width, work.top + 45 + height};
    PositionForTest(card, initial);

    RECT moving{work.left + 3, work.top + 4, work.left + 3 + width, work.top + 4 + height};
    const LRESULT movingResult = SendMessageW(card.hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&moving));
    CheckUi(movingResult == TRUE && moving.left == work.left && moving.top == work.top &&
        Width(moving) == width && Height(moving) == height,
        "WM_MOVING snaps to work-area edges without changing card size");
    CheckUi(EqualRect(WindowRect(card.hwnd), initial),
        "WM_MOVING edits the proposed rectangle without recursively moving HWND");

    desk::CardState state;
    state.id = app.nextId++;
    state.title = L"snap fixture";
    state.monitor = monitor.device;
    state.x = 2;
    state.y = 3;
    state.width = 228;
    state.height = 190;
    Card* peer = CreateCard(std::move(state));
    CheckUi(peer && IsWindow(peer->hwnd), "create native startup-snap fixture");
    if (peer) {
        RECT startup = WindowRect(peer->hwnd);
        CheckUi(startup.left == work.left+MulDiv(2,monitor.dpi,96) && startup.top == work.top+MulDiv(3,monitor.dpi,96),
            "opening a card retains the user's custom near-edge gap");
        ShowWindow(peer->hwnd, SW_HIDE);
        RECT anchor{work.left + width + 60, work.top + 40,
            work.left + width * 2 + 60, work.top + 40 + height};
        PositionForTest(*peer, anchor);
        moving = {anchor.left - 3 - width, anchor.top + 3,
            anchor.left - 3, anchor.top + 3 + height};
        SendMessageW(card.hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&moving));
        CheckUi(moving.right == anchor.left && moving.top == anchor.top &&
            Width(moving) == width && Height(moving) == height,
            "WM_MOVING snaps card contact and aligns its top edge to a peer");

        RECT sizing{work.left + 35, anchor.top, anchor.left - 4, anchor.top + height};
        const RECT beforeSizing = sizing;
        const LRESULT sizingResult = SendMessageW(card.hwnd, WM_SIZING, WMSZ_RIGHT,
            reinterpret_cast<LPARAM>(&sizing));
        CheckUi(sizingResult == TRUE && sizing.right == anchor.left &&
            sizing.left == beforeSizing.left && sizing.top == beforeSizing.top &&
            sizing.bottom == beforeSizing.bottom,
            "WM_SIZING snaps the dragged edge while preserving fixed edges");

        card.state.locked = true;
        RECT lockedMove{work.left + 3, work.top + 4,
            work.left + 3 + width, work.top + 4 + height};
        const RECT beforeMove = lockedMove;
        SendMessageW(card.hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&lockedMove));
        sizing = beforeSizing;
        SendMessageW(card.hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&sizing));
        CheckUi(EqualRect(lockedMove, beforeMove) && EqualRect(sizing, beforeSizing),
            "locked cards leave move and resize proposals unchanged");
        HandleCommand(peer, RemoveCard);
    }
    card.state.locked = wasLocked;
    const HWND foreground=GetForegroundWindow();
    desk::SnapGuides guides; guides.vertical=guides.horizontal=true;
    guides.verticalRect={work.left+40,work.top+40,work.left+41,work.top+240};
    guides.horizontalRect={work.left+40,work.top+40,work.left+240,work.top+41};
    ShowSnapGuides(guides);
    for(HWND guide:app.snapGuides) {
        const LONG_PTR style=GetWindowLongPtrW(guide,GWL_EXSTYLE);
        CheckUi(guide && IsWindowVisible(guide) && (style&WS_EX_TRANSPARENT) && (style&WS_EX_NOACTIVATE) &&
            SendMessageW(guide,WM_NCHITTEST,0,0)==HTTRANSPARENT,
            "temporary alignment guides are visible, click-through and non-activating");
    }
    CheckUi(GetForegroundWindow()==foreground,"alignment guides do not steal foreground focus");
    const auto depth=app.modalDepth;
    card.state.locked=false;
    SendMessageW(card.hwnd,WM_ENTERSIZEMOVE,0,0); card.pointerDrag=false;
    moving={work.left+3,work.top+4,work.left+3+width,work.top+4+height};
    const RECT exact=moving;
    SendMessageW(card.hwnd,WM_MOVING,0,reinterpret_cast<LPARAM>(&moving));
    CheckUi(EqualRect(moving,exact),"keyboard positioning bypasses snap for fine adjustments");
    SendMessageW(card.hwnd,WM_EXITSIZEMOVE,0,0);
    CheckUi(app.modalDepth==depth && !card.inSizeMove &&
        std::none_of(std::begin(app.snapGuides),std::end(app.snapGuides),[](HWND guide){return IsWindowVisible(guide)!=FALSE;}),
        "finishing movement removes guides and releases its modal scope");
    card.state.locked=wasLocked;
    // Feed real WM_MOVING handlers incremental native proposals while the
    // pointer advances one pixel at a time. No system pointer input is sent.
    card.state.locked=false;
    PositionForTest(card,{work.left,work.top,work.left+width,work.top+height});
    uiTestPointer={work.left+50,work.top+20};uiTestPointerInjected=true;
    SendMessageW(card.hwnd,WM_ENTERSIZEMOVE,0,0);
    CheckUi(card.pointerDrag,"mouse move loop records its initial pointer anchor");
    RECT incremental=WindowRect(card.hwnd);
    const int threshold=Dip(card,12);
    for(int step=1;step<=threshold+30;++step) {
        ++uiTestPointer.x;++uiTestPointer.y;
        OffsetRect(&incremental,1,1);
        SendMessageW(card.hwnd,WM_MOVING,0,reinterpret_cast<LPARAM>(&incremental));
        const LONG delta=step<=threshold?0:step;
        CheckUi(incremental.left==work.left+delta && incremental.top==work.top+delta &&
            Width(incremental)==width && Height(incremental)==height,
            "native mouse move escapes the corner under repeated one-pixel movement");
    }
    SendMessageW(card.hwnd,WM_EXITSIZEMOVE,0,0);
    uiTestPointerInjected=false;card.state.locked=wasLocked;
    for (size_t i = 0; i < originalRects.size(); ++i)
        PositionForTest(*app.cards[i], originalRects[i]);
}

void TestControllerCommands() {
    for (auto& card : app.cards) ShowWindow(card->hwnd, SW_HIDE);
    SendMessageW(app.controller, kShow, 0, 0);
    CheckUi(std::all_of(app.cards.begin(), app.cards.end(), [](const std::unique_ptr<Card>& card) {
        return IsWindowVisible(card->hwnd) != FALSE;
    }), "desktop open command shows the running application's cards");
    for (auto& card : app.cards) ShowWindow(card->hwnd, SW_HIDE);

    const size_t before = app.cards.size();
    const uint32_t previousId = app.nextId;
    SendMessageW(app.controller, kNewCard, 0, 0);
    CheckUi(app.cards.size() == before + 1 && app.nextId > previousId,
        "internal new-card command creates exactly one card in a running instance");
    if (app.cards.size() == before + 1) {
        Card* created = app.cards.back().get();
        CheckUi(IsWindowVisible(created->hwnd) != FALSE,
            "newly created card is immediately visible");
        HandleCommand(created, RemoveCard);
    }
}

void TestVisualOwnership(Card& card) {
    CheckUi(!card.items.empty(), "visual routing has a target item");
    if (card.items.empty()) return;
    const auto originalLabel = card.items[0].label;
    auto* stale = new VisualResult;
    stale->cardId = card.state.id;
    stale->token = card.items[0].token + 1000000;
    stale->visual.icon = TestIcon();
    stale->visual.label = L"stale visual";
    HICON staleIcon = stale->visual.icon;
    SendMessageW(app.controller, kVisual, 0, reinterpret_cast<LPARAM>(stale));
    CheckUi(card.items[0].label == originalLabel && !IconAlive(staleIcon),
        "stale item result is ignored and its owned icon is freed");

    auto* orphan = new VisualResult;
    orphan->cardId = app.nextId + 1000;
    orphan->token = card.items[0].token;
    orphan->visual.icon = TestIcon();
    HICON orphanIcon = orphan->visual.icon;
    SendMessageW(app.controller, kVisual, 0, reinterpret_cast<LPARAM>(orphan));
    CheckUi(!IconAlive(orphanIcon), "result for removed card frees its owned icon");

    auto* accepted = new VisualResult;
    accepted->cardId = card.state.id;
    accepted->token = card.items[0].token;
    accepted->visual.icon = TestIcon();
    accepted->visual.label = L"accepted visual";
    HICON acceptedIcon = accepted->visual.icon;
    SendMessageW(app.controller, kVisual, 0, reinterpret_cast<LPARAM>(accepted));
    CheckUi(card.items[0].label == L"accepted visual" && card.items[0].icon == acceptedIcon &&
        IconAlive(acceptedIcon), "matching controller result transfers icon ownership to item");

    auto* action = new VisualResult;
    action->cardId = card.state.id;
    action->action = Open;
    action->okay = false;
    SendMessageW(app.controller, kVisual, 0, reinterpret_cast<LPARAM>(action));
    CheckUi(!card.status.empty(), "failed action result updates card without launching anything");
}

bool SavePreviewPng(HBITMAP image, const std::wstring& path) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;
    bool saved = false;
    {
        Gdiplus::Bitmap bitmap(image, nullptr);
        const CLSID pngEncoder = {0x557cf406, 0x1a04, 0x11d3,
            {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        saved = bitmap.GetLastStatus() == Gdiplus::Ok &&
            bitmap.Save(path.c_str(), &pngEncoder, nullptr) == Gdiplus::Ok;
    }
    Gdiplus::GdiplusShutdown(token);
    return saved;
}

void ExportRenderedPreview() {
    const auto executable = desk::ExecutablePath();
    const auto testsDirectory = executable.substr(0, executable.find_last_of(L"\\/"));
    const auto buildDirectory = testsDirectory.substr(0, testsDirectory.find_last_of(L"\\/"));
    const auto projectDirectory = buildDirectory.substr(0, buildDirectory.find_last_of(L"\\/"));
    wchar_t windows[MAX_PATH]{};
    CheckUi(GetWindowsDirectoryW(windows, MAX_PATH) != 0, "locate safe preview files");
    desk::CardState grid;
    grid.id = app.nextId++;
    grid.title = L"\u5e38\u7528\u5e94\u7528";
    grid.paths = {std::wstring(windows) + L"\\explorer.exe",
        std::wstring(windows) + L"\\System32\\notepad.exe",
        std::wstring(windows) + L"\\System32\\cmd.exe",
        std::wstring(windows) + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
        projectDirectory + L"\\dist\\MintDesk.exe", projectDirectory,
        projectDirectory + L"\\README.md", projectDirectory + L"\\build.ps1"};
    desk::CardState list;
    list.id = app.nextId++;
    list.title = L"\u9879\u76ee\u6587\u4ef6";
    list.layout = desk::Layout::List;
    list.locked = true;
    list.paths = {projectDirectory + L"\\src", projectDirectory + L"\\tests",
        projectDirectory + L"\\assets", projectDirectory + L"\\README.md",
        projectDirectory + L"\\build.ps1", projectDirectory + L"\\CMakeLists.txt",
        projectDirectory + L"\\src\\main.cpp", projectDirectory + L"\\src\\state.cpp"};

    app.worker = StartWorker();
    Card* left = CreateCard(std::move(grid));
    Card* right = CreateCard(std::move(list));
    CheckUi(left && right, "create preview grid and list cards");
    if (!left || !right) { StopWorker(app.worker); return; }
    constexpr int width = 440, height = 600, gap = 24;
    for (Card* card : {left, right}) {
        card->placing = true;
        SetWindowPos(card->hwnd, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        card->placing = false;
        card->dpi = 96;
        Fonts(*card);
        ReloadVisuals(*card);
    }
    const ULONGLONG deadline = GetTickCount64() + 2000;
    bool iconsReady = false;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        iconsReady = true;
        for (Card* card : {left, right})
            for (const auto& item : card->items) if (!item.icon) iconsReady = false;
        if (!iconsReady) Sleep(10);
    } while (!iconsReady && GetTickCount64() < deadline);
    CheckUi(iconsReady, "safe local preview icons arrive within two seconds");
    StopWorker(app.worker);

    HDC screen = GetDC(nullptr);
    HDC canvas = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, width * 2 + gap * 3, height + gap * 2);
    ReleaseDC(nullptr, screen);
    CheckUi(canvas && image, "allocate native preview canvas");
    if (canvas && image) {
        HGDIOBJ previous = SelectObject(canvas, image);
        Fill(canvas, RECT{0, 0, width * 2 + gap * 3, height + gap * 2}, RGB(9, 13, 20));
        SetViewportOrgEx(canvas, gap, gap, nullptr);
        Paint(*left, canvas);
        SetViewportOrgEx(canvas, width + gap * 2, gap, nullptr);
        Paint(*right, canvas);
        SetViewportOrgEx(canvas, 0, 0, nullptr);
        CheckUi(GetPixel(canvas, gap + width / 2, gap + height / 2) == kBg,
            "preview uses the production card painter");
        SelectObject(canvas, previous);
        const std::wstring destination = buildDirectory + L"\\ui-preview.png";
        CheckUi(SavePreviewPng(image, destination), "export production grid/list renderer as PNG");
        std::puts("Rendered preview: build/ui-preview.png");

        // Export the actual layered-window surfaces over a visible backdrop as
        // well: flattening the painter over kBg would conceal an alpha bug.
        SelectObject(canvas, image);
        constexpr int checker = 28;
        for (int y = 0; y < height + gap * 2; y += checker)
            for (int x = 0; x < width * 2 + gap * 3; x += checker)
                Fill(canvas, RECT{x, y, x + checker, y + checker},
                    ((x / checker + y / checker) & 1) ? RGB(38, 51, 63) : RGB(56, 71, 86));
        desk::AlphaSurface leftAlpha, rightAlpha;
        const bool alphaReady = RenderCardSurface(*left, leftAlpha) &&
            RenderCardSurface(*right, rightAlpha);
        CheckUi(alphaReady && leftAlpha.BlitOver(canvas, gap, gap) &&
            rightAlpha.BlitOver(canvas, width + gap * 2, gap),
            "preview composites the actual native ARGB surfaces over a visible checkerboard");
        CheckUi(GetPixel(canvas, gap + width / 2, gap + height / 2) != kBg,
            "alpha preview exposes the backdrop through otherwise empty card background");
        SelectObject(canvas, previous);
        CheckUi(SavePreviewPng(image, buildDirectory + L"\\ui-alpha-preview.png"),
            "export background-only transparency preview as PNG");
        std::puts("Rendered alpha preview: build/ui-alpha-preview.png");
    }
    if (image) DeleteObject(image);
    if (canvas) DeleteDC(canvas);
    HandleCommand(left, RemoveCard);
    HandleCommand(right, RemoveCard);
}

void TestCloseRestoreFailureKeepsInstance(const Fixture& fixture) {
    const auto root = fixture.directory + L"\\close-failure";
    const auto config = root + L"\\Config", desktop = root + L"\\Desktop";
    const auto common = root + L"\\Public", storage = root + L"\\Managed";
    const auto stateDirectory = root + L"\\State";
    bool directoriesReady = true;
    for (const auto& directory : {root, config, desktop, common, storage, stateDirectory}) {
        const bool created = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
        CheckUi(created, "create explicitly isolated close-failure fixture directory");
        directoriesReady = directoriesReady && created;
    }
    const auto original = desktop + L"\\close-owned.txt";
    const auto additional = desktop + L"\\gate-protected.txt";
    const char payload[] = "locked desktop close must preserve this payload\n";
    auto writeOwnedFixture = [&](const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool okay = WriteFile(file, payload, sizeof(payload) - 1, &written, nullptr) &&
            written == sizeof(payload) - 1;
        CloseHandle(file);
        return okay;
    };
    auto hasOriginalPayload = [&](const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        char bytes[sizeof(payload)]{};
        DWORD read = 0;
        const bool okay = ReadFile(file, bytes, sizeof(payload) - 1, &read, nullptr) &&
            read == sizeof(payload) - 1 && std::memcmp(bytes, payload, sizeof(payload) - 1) == 0;
        CloseHandle(file);
        return okay;
    };
    const bool filesReady = directoriesReady && writeOwnedFixture(original) && writeOwnedFixture(additional);
    CheckUi(filesReady, "create only dedicated temporary payloads for failed shutdown and acquisition gate");
    const bool initialized = filesReady &&
        desk::testing::InitializeDesktopItemsAt(config, desktop, common, storage);
    CheckUi(initialized, "inject isolated desktop ownership roots before exercising production close control flow");
    std::wstring managed, retriedManaged;
    Card* card = nullptr;
    HANDLE locked = INVALID_HANDLE_VALUE;
    const bool previousReady = app.desktopReady;
    const bool previousEnding = app.sessionEnding;
    if (initialized) {
        const bool acquired = desk::ManageDesktopItem(original) == desk::DesktopManageResult::Managed;
        CheckUi(acquired, "acquire the isolated desktop payload before simulating a sharing violation");
        managed = desk::ResolveDesktopItem(original);
        if (acquired) locked = CreateFileW(managed.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CheckUi(locked != INVALID_HANDLE_VALUE, "hold managed payload open without FILE_SHARE_DELETE");
        desk::CardState state;
        state.id = app.nextId++;
        state.title = L"close failure fixture";
        state.paths = {original};
        card = CreateCard(std::move(state));
        CheckUi(card != nullptr, "create a temporary card referencing the locked desktop payload");
        if (card && locked != INVALID_HANDLE_VALUE) {
            ShowWindow(card->hwnd, SW_HIDE);
            const HWND controller = app.controller;
            const size_t cardCount = app.cards.size();
            const HWND cardWindow = card->hwnd;
            const HWINEVENTHOOK foregroundHook = app.foregroundHook;
            const bool trayAdded = app.trayAdded;
            const unsigned beforeSaves = uiTestSaveCalls, beforeRefreshes = uiTestMenuRefreshCalls;
            uiTestStateDirectory = stateDirectory;
            app.desktopReady = true;
            app.sessionEnding = false;
            app.preview = false;
            SendMessageW(controller, WM_CLOSE, 0, 0);
            const bool retained = app.controller == controller && IsWindow(controller) &&
                IsWindow(cardWindow) && app.cards.size() == cardCount;
            CheckUi(retained && !app.quitting && !app.pendingQuit && !app.desktopReady,
                "failed desktop restoration cancels normal shutdown and gates further desktop acquisition");
            CheckUi(app.foregroundHook == foregroundHook && app.trayAdded == trayAdded &&
                app.worker && WaitForSingleObject(app.worker->thread, 0) == WAIT_TIMEOUT,
                "failed close retains foreground/tray lifecycle and restarts the usable icon worker");
            desk::AppState saved;
            CheckUi(uiTestSaveCalls == beforeSaves + 1 &&
                desk::testing::LoadStateFromDirectory(stateDirectory, saved) && saved.cards.size() == cardCount,
                "failed close saves recoverable layout only through the isolated test-state wrapper");
            CheckUi(uiTestMenuRefreshCalls == beforeRefreshes,
                "cancelled shutdown never reaches desktop-menu exit refresh");
            CheckUi(GetFileAttributesW(original.c_str()) == INVALID_FILE_ATTRIBUTES &&
                hasOriginalPayload(managed), "sharing-violation failure leaves the complete payload recoverable in managed storage");
            if (retained) {
                AddPaths(*card, {additional});
                CheckUi(!app.desktopReady && card->state.paths.size() == 2 &&
                    std::find(card->state.paths.begin(), card->state.paths.end(), additional) != card->state.paths.end() &&
                    !desk::IsManagedDesktopItem(additional) && desk::ResolveDesktopItem(additional) == additional &&
                    hasOriginalPayload(additional),
                    "desktopReady=false accepts a reference while leaving a new desktop file in place");
            }
            app.preview = true;
            KillTimer(controller, kSaveTimer);
            uiTestStateDirectory.clear();
        }
        if (locked != INVALID_HANDLE_VALUE) { CloseHandle(locked); locked = INVALID_HANDLE_VALUE; }
        CheckUi(desk::RestoreDesktopItems() && hasOriginalPayload(original) && hasOriginalPayload(additional),
            "releasing the sharing lock restores the original path and preserves both payloads exactly");
        if(card&&IsWindow(card->hwnd)&&card->state.paths.size()==2) {
            app.desktopReady=true;app.preview=false;
            AddPaths(*card,{additional});
            app.preview=true;
            KillTimer(app.controller,kSaveTimer);
            retriedManaged=desk::ResolveDesktopItem(additional);
            CheckUi(card->state.paths.size()==2&&desk::IsManagedDesktopItem(additional)&&
                GetFileAttributesW(additional.c_str())==INVALID_FILE_ATTRIBUTES&&hasOriginalPayload(retriedManaged),
                "dragging an existing previously failed reference retries its import without duplicating the card item");
            CheckUi(desk::RestoreDesktopItems()&&hasOriginalPayload(additional),"retried import restores its original bytes and desktop path");
        }
    }
    app.preview = true;
    StopWorker(app.worker);
    desk::ClearDesktopItems();
    app.desktopReady = previousReady;
    app.sessionEnding = previousEnding;
    uiTestStateDirectory.clear();
    if (card && std::any_of(app.cards.begin(), app.cards.end(), [&](const std::unique_ptr<Card>& item) {
        return item.get() == card;
    })) HandleCommand(card, RemoveCard);
    DeleteFileW(original.c_str());
    DeleteFileW(additional.c_str());
    for (const auto* name : {L"desktop-items.dat", L"desktop-items.backup.dat",
            L"desktop-items.dat.tmp", L"desktop-items.backup.dat.tmp"})
        DeleteFileW((config + L"\\" + name).c_str());
    DeleteFileW((stateDirectory + L"\\state.dat").c_str());
    DeleteFileW((stateDirectory + L"\\state.backup.dat").c_str());
    if (managed.find(storage + L"\\") == 0)
        RemoveDirectoryW(managed.substr(0, managed.find_last_of(L'\\')).c_str());
    if(retriedManaged.find(storage+L"\\")==0)
        RemoveDirectoryW(retriedManaged.substr(0,retriedManaged.find_last_of(L'\\')).c_str());
    for (const auto& directory : {stateDirectory, storage, common, desktop, config, root})
        CheckUi(RemoveDirectoryW(directory.c_str()) != FALSE,
            "clean up failed-close fixture using only known files and empty temporary directories");
}

void TestRemoveAndQuit(Card& card, const Fixture& fixture) {
    const size_t itemCount = card.items.size();
    card.selected = 0;
    HandleCommand(&card, RemoveItem);
    CheckUi(card.items.size() + 1 == itemCount && card.state.paths.size() == card.items.size() &&
        fixture.originalExists(), "removing item only removes its reference");

    desk::CardState extra;
    extra.id = app.nextId++;
    extra.title = L"removal fixture";
    extra.paths = {fixture.original};
    Card* removed = CreateCard(std::move(extra));
    CheckUi(removed && IsWindow(removed->hwnd), "create extra native card");
    if (removed) {
        const size_t count = app.cards.size();
        const HWND removedWindow = removed->hwnd;
        HandleCommand(removed, RemoveCard);
        CheckUi(app.cards.size() + 1 == count && !IsWindow(removedWindow) && fixture.originalExists(),
            "removing card destroys HWND and preserves referenced original");
    }

    const HWND controller = app.controller;
    const size_t count = app.cards.size();
    {
        ModalScope outer;
        {
            ModalScope inner;
            SendMessageW(controller, WM_CLOSE, 0, 0);
            CheckUi(app.pendingQuit && !app.quitting && app.modalDepth == 2 &&
                IsWindow(controller) && app.cards.size() == count,
                "quit is deferred while nested modal scopes retain card objects");
        }
        CheckUi(app.modalDepth == 1 && IsWindow(controller), "inner modal return keeps controller alive");
    }
    MSG close{};
    const bool posted = PeekMessageW(&close, controller, WM_CLOSE, WM_CLOSE, PM_REMOVE) != FALSE;
    CheckUi(posted && app.modalDepth == 0, "outer modal return posts deferred quit");
    if (posted) DispatchMessageW(&close);
    else SendMessageW(controller, WM_CLOSE, 0, 0);
    CheckUi(app.quitting && !app.controller && app.cards.empty() && !IsWindow(controller),
        "deferred quit cleans up native card and controller windows");
    CheckUi(fixture.originalExists(), "quitting preserves original file");
}
} // namespace

int wmain() {
    const HRESULT com = OleInitialize(nullptr);
    app.preview = true;
    Fixture fixture;
    CheckUi(fixture.ready, "create isolated original-file fixture");
    try {
        const bool initialized = Initialize(GetModuleHandleW(nullptr));
        CheckUi(initialized, "initialize preview windows (close any running /preview instance first)");
        if (initialized) {
            for (auto& card : app.cards) ShowWindow(card->hwnd, SW_HIDE);
            // Test lexical references without invoking installed shell handlers.
            StopWorker(app.worker);
            CheckUi(IsWindow(app.controller) && !app.cards.empty() && IsWindow(app.cards[0]->hwnd),
                "controller and default card HWNDs exist");
            if (!app.cards.empty() && fixture.ready) {
                Card& card = *app.cards.front();
                TestReferencesAndInput(card, fixture);
                TestSelectionDismissal(fixture);
                TestSavedUserLayout(fixture);
                TestCompactHeaderAndInput(card);
                TestDesktopLayerAndKeyboard(fixture);
                TestGlassBackdropLifecycle();
                TestBackgroundOnlyTransparency(fixture);
                TestCollapseAndRestore(fixture);
                TestStableTitleEditing();
                TestCoreMenus(card);
                TestBoundedAnimation(card);
                TestNativeDropIntoCard(fixture);
                TestManagedDesktopDropIdentity(fixture);
                TestSameCardReordering(fixture);
                TestHiddenExternalCopy(fixture);
                TestDeleteWhilePressed(fixture);
                TestFullCardRejectsNewDrop(fixture);
                TestDpiAndDisplay(card);
                TestNativeSnapping(card);
                TestControllerCommands();
                TestVisualOwnership(card);
                ExportRenderedPreview();
                TestCloseRestoreFailureKeepsInstance(fixture);
                const bool retainedOriginal = app.controller && IsWindow(app.controller) &&
                    std::any_of(app.cards.begin(), app.cards.end(), [&](const std::unique_ptr<Card>& item) {
                        return item.get() == &card;
                    });
                CheckUi(retainedOriginal, "close-failure regression preserves the original fixtures for normal quit testing");
                if (retainedOriginal) TestRemoveAndQuit(card, fixture);
            }
        }
    } catch (...) {
        CheckUi(false, "integration harness completed without an exception");
    }
    app.preview = true;
    app.modalDepth = 0;
    if (app.controller) SendMessageW(app.controller, WM_CLOSE, 0, 0);
    if (app.icon) { DestroyIcon(app.icon); app.icon = nullptr; }
    if (app.singleton) { CloseHandle(app.singleton); app.singleton = nullptr; }
    if (SUCCEEDED(com)) OleUninitialize();
    if (!testFailures) std::puts("UI integration tests passed.");
    return testFailures ? 1 : 0;
}
