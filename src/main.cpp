#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "state.h"
#include "shell_items.h"
#include "snap.h"
#include "desktop_menu.h"
#include "dragdrop.h"
#include "alpha_surface.h"
#include "desktop_items.h"
#include "glass_backdrop.h"
#include "file_helper.h"
#include "hidden_items.h"
#include "startup_integration.h"

// MintDesk uses ordinary, bottom-positioned tool windows. It never reparents
// Explorer/WorkerW windows or installs a shell hook. Per-file visibility is
// journaled separately; the global desktop icon settings are never changed.
namespace {
constexpr wchar_t kCardClass[] = L"MintDesk.Card.1";
constexpr wchar_t kControllerClass[] = L"MintDesk.Controller.1";
constexpr UINT kTray = WM_APP + 1, kVisual = WM_APP + 2;
constexpr UINT kLower = WM_APP + 3, kShow = WM_APP + 4;
constexpr UINT kNewCard = WM_APP + 5;
constexpr UINT_PTR kSaveTimer = 1, kDisplayTimer = 2;
constexpr UINT_PTR kAnimationTimer = 10, kStatusTimer = 11;
constexpr UINT_PTR kCaretTimer = 12, kDragScrollTimer = 13;
constexpr ULONGLONG kFeedbackDuration = 180;
constexpr size_t kMaxItems = 2048, kMaxCards = 64, kMaxTotalItems = 20000;
constexpr COLORREF kBg = RGB(20, 24, 30), kLine = RGB(48, 55, 65);
constexpr COLORREF kText = RGB(230, 235, 241), kMuted = RGB(135, 147, 162);
constexpr COLORREF kAccent = RGB(85, 211, 177), kHover = RGB(37, 46, 56);

enum Command : UINT {
    AddFiles = 100, AddFolder, Rename, Grid, List, Lock, NewCard, RemoveCard,
    Autostart, Quit, ShowCards, Open, Reveal, RemoveItem,
    OpacitySolid, Opacity50, Opacity25, OpacityTransparent, DesktopMenu, DesktopPermission
};

struct Item {
    HICON icon = nullptr;
    std::wstring label;
    uint64_t token = 0;
};
struct Card {
    desk::CardState state;
    desk::GlassBackdrop glass;
    HWND hwnd = nullptr, editor = nullptr;
    HFONT font = nullptr, titleFont = nullptr, smallFont = nullptr;
    UINT dpi = 96;
    std::vector<Item> items;
    int selected = -1, hovered = -1, scroll = 0;
    int wheelRemainder = 0;
    int headerHover = 0, headerPressed = 0, mouseDownItem = -1;
    POINT mouseDownPoint{}, dropPoint{};
    bool dropHovered = false, feedbackActive = false, feedbackSuccess = true;
    bool oleDropRegistered = false, draggingOut = false;
    bool caretVisible = true;
    ULONGLONG feedbackStart = 0, statusUntil = 0;
    uint64_t lastAddedToken = 0;
    bool tracking = false, placing = false, draggingScroll = false, arrangingZ = false;
    int scrollGrab = 0;
    std::wstring status;
    ~Card() {
        for (auto& item : items) if (item.icon) DestroyIcon(item.icon);
        if (font) DeleteObject(font);
        if (titleFont) DeleteObject(titleFont);
        if (smallFont) DeleteObject(smallFont);
    }
};
struct Job { uint32_t cardId; uint64_t token; int size; std::wstring path; UINT action = 0; std::wstring label; };
struct VisualResult {
    uint32_t cardId = 0;
    uint64_t token = 0;
    desk::ShellVisual visual;
    UINT action = 0;
    bool okay = false;
    ~VisualResult() { if (visual.icon) DestroyIcon(visual.icon); }
};
struct Worker {
    CRITICAL_SECTION mutex;
    HANDLE event = nullptr, thread = nullptr;
    std::deque<Job> queue;
    std::atomic<bool> stopping{false};
};
struct Application {
    HINSTANCE instance = nullptr;
    HWND controller = nullptr;
    HICON icon = nullptr;
    HANDLE singleton = nullptr;
    HWINEVENTHOOK foregroundHook = nullptr;
    UINT taskbarCreated = 0;
    std::vector<std::unique_ptr<Card>> cards;
    uint32_t nextId = 1;
    uint64_t nextToken = 1;
    BYTE opacity = 0;
    Worker* worker = nullptr;
    Worker* actions = nullptr;
    unsigned modalDepth = 0;
    bool pendingQuit = false, preview = false, newCardRequest = false;
    bool quitting = false, displayChanging = false, trayAdded = false;
    bool desktopReady = true, sessionEnding = false;
    uint32_t dragSourceId = 0;
    const std::wstring* dragReference = nullptr;
    bool dragHandledInside = false;
    int exitCode = 0;
} app;

int Dip(const Card& card, int value) { return MulDiv(value, card.dpi, 96); }
int Width(const RECT& r) { return r.right - r.left; }
int Height(const RECT& r) { return r.bottom - r.top; }
bool Inside(const RECT& r, POINT p) { return PtInRect(&r, p) != FALSE; }
void Invalidate(Card& c) { InvalidateRect(c.hwnd, nullptr, FALSE); }
void Notify(Card& c,const std::wstring& message);
void Dirty() { if (!app.quitting && !app.displayChanging && !app.preview) SetTimer(app.controller, kSaveTimer, 350, nullptr); }

struct CriticalLock {
    CRITICAL_SECTION& mutex;
    explicit CriticalLock(CRITICAL_SECTION& value) : mutex(value) { EnterCriticalSection(&mutex); }
    ~CriticalLock() { LeaveCriticalSection(&mutex); }
};

// Native pickers and menus run nested message loops. Defer quit until all such
// scopes have returned, keeping card objects alive during reentrant messages.
struct ModalScope {
    ModalScope() { ++app.modalDepth; }
    ~ModalScope() {
        if (!--app.modalDepth && app.pendingQuit) PostMessageW(app.controller, WM_CLOSE, 0, 0);
    }
};
struct CardDragScope {
    CardDragScope(Card& source,const std::wstring& reference) {
        app.dragSourceId=source.state.id;app.dragReference=&reference;app.dragHandledInside=false;
    }
    void Finish(){app.dragSourceId=0;app.dragReference=nullptr;app.dragHandledInside=false;}
    ~CardDragScope(){Finish();}
};

struct ExternalVisibilityScope {
    std::wstring actual;
    bool exposed=false;
    static bool Prepare(void* context) {
        auto& self=*static_cast<ExternalVisibilityScope*>(context);
        if(!desk::IsHiddenDesktopItem(self.actual))return true;
        if(!desk::ReleaseHiddenDesktopItem(self.actual))return false;
        self.exposed=true;return true;
    }
    ~ExternalVisibilityScope(){if(exposed&&!app.quitting)desk::HideDesktopItem(actual);}
};

desk::DesktopManageResult ManageDesktopPath(const std::wstring& path) {
    if(app.preview)return desk::DesktopManageResult::Reference;
    if(!app.desktopReady)return desk::DesktopManageResult::Failed;
    auto result=desk::ManageDesktopItem(path);
    if(result==desk::DesktopManageResult::RequiresPermission) {
        ModalScope modal;
        if(desk::EnsureFileHelper(app.controller)&&!app.pendingQuit)result=desk::ManageDesktopItem(path);
    }
    return result;
}

bool RestoreDesktopWithPermission() {
    if(desk::RestoreDesktopItems())return true;
    if(!desk::DesktopItemsNeedPermission())return false;
    ModalScope modal;
    return desk::EnsureFileHelper(app.controller,true)&&desk::RestoreDesktopItems();
}
bool ReleaseDesktopWithPermission(const std::wstring& reference) {
    if(desk::ReleaseDesktopItem(reference))return true;
    if(!desk::DesktopItemsNeedPermission())return false;
    ModalScope modal;
    return desk::EnsureFileHelper(app.controller,true)&&desk::ReleaseDesktopItem(reference);
}

template<class Function> Function User32Function(const char* name) {
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

UINT WindowDpi(HWND hwnd) {
    using GetDpi = UINT(WINAPI*)(HWND);
    static auto fn = User32Function<GetDpi>("GetDpiForWindow");
    return fn ? fn(hwnd) : 96;
}

void Fonts(Card& c) {
    if (c.font) DeleteObject(c.font);
    if (c.titleFont) DeleteObject(c.titleFont);
    if (c.smallFont) DeleteObject(c.smallFont);
    auto make = [&](int size, int weight) {
        return CreateFontW(-Dip(c, size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    c.font = make(12, FW_NORMAL);
    c.titleFont = make(12, FW_SEMIBOLD);
    c.smallFont = make(10, FW_NORMAL);
}

bool DesktopSurface(HWND window) {
    wchar_t name[64]{};
    GetClassNameW(window,name,ARRAYSIZE(name));
    return !wcscmp(name,kCardClass)||!wcscmp(name,L"MintDesk.Backdrop.1");
}

HWND DesktopIconHost() {
    // Wallpaper Engine creates additional WorkerW windows behind the icons.
    // Only the Explorer root that actually owns DefView marks the icon layer.
    struct Search { DWORD process=0; HWND host=nullptr; } search;
    const HWND shell=GetShellWindow();
    if (!shell) return nullptr;
    GetWindowThreadProcessId(shell,&search.process);
    if (!search.process) return nullptr;
    EnumWindows([](HWND window,LPARAM parameter)->BOOL {
        auto& result=*reinterpret_cast<Search*>(parameter);
        DWORD process=0; GetWindowThreadProcessId(window,&process);
        if (process!=result.process||!IsWindowVisible(window)) return TRUE;
        wchar_t name[64]{}; GetClassNameW(window,name,ARRAYSIZE(name));
        if (wcscmp(name,L"Progman")&&wcscmp(name,L"WorkerW")) return TRUE;
        if (!FindWindowExW(window,nullptr,L"SHELLDLL_DefView",nullptr)) return TRUE;
        result.host=window; return FALSE;
    },reinterpret_cast<LPARAM>(&search));
    return search.host;
}

bool DesktopAnchor(HWND& anchor) {
    const HWND desktop=DesktopIconHost();
    if (!desktop) return false;
    anchor=GetWindow(desktop,GW_HWNDPREV);
    // Keep all our foreground/background surfaces in one band directly above
    // the desktop, preserving ordinary windows even when they share our PID.
    while (anchor&&DesktopSurface(anchor)) anchor=GetWindow(anchor,GW_HWNDPREV);
    if (!anchor||(GetWindowLongPtrW(anchor,GWL_EXSTYLE)&WS_EX_TOPMOST))
        anchor=HWND_NOTOPMOST;
    return true;
}

void PlaceAtDesktop(HWND window, const RECT* rect = nullptr) {
    HWND anchor=nullptr;
    const bool anchored=DesktopAnchor(anchor);
    SetWindowPos(window, anchor, rect ? rect->left : 0, rect ? rect->top : 0,
        rect ? Width(*rect) : 0, rect ? Height(*rect) : 0,
        SWP_NOACTIVATE | (rect ? 0 : SWP_NOMOVE|SWP_NOSIZE) | (anchored?0:SWP_NOZORDER));
}

void FocusCard(Card& card,HWND target) {
    // Request keyboard focus only for explicit input, after automatic mouse
    // activation has been suppressed. The z-order guard also covers these API
    // activation requests, so focused cards remain behind ordinary windows.
    SetForegroundWindow(card.hwnd); SetActiveWindow(card.hwnd);
    SetFocus(target); PlaceAtDesktop(card.hwnd);
}

void EnforceDesktopBand(Card& card) {
    if (card.arrangingZ||!card.hwnd) return;
    const HWND desktop=DesktopIconHost();
    if (!desktop) return;
    // Check from the desktop upward: the card is already in its band if no
    // ordinary window separates it from the icon host. Most geometry changes
    // therefore need no additional SetWindowPos call.
    bool inBand=false;
    for (HWND window=GetWindow(desktop,GW_HWNDPREV);window;window=GetWindow(window,GW_HWNDPREV)) {
        if (window==card.hwnd) { inBand=true; break; }
        if (!DesktopSurface(window)) break;
    }
    if (inBand&&!(GetWindowLongPtrW(card.hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)) return;
    // Some activation APIs apply a final promotion after WINDOWPOSCHANGING.
    // Correct that completed change synchronously, guarding the nested message.
    card.arrangingZ=true;
    PlaceAtDesktop(card.hwnd);
    card.arrangingZ=false;
}

void SnapCardRect(Card& card, RECT& proposed, desk::SnapEdge edge) {
    if (card.state.locked) return;
    const auto monitors=desk::EnumerateMonitors();
    std::vector<RECT> workAreas, peers;
    workAreas.reserve(monitors.size()); peers.reserve(app.cards.size());
    for (const auto& monitor:monitors) workAreas.push_back(monitor.work);
    for (auto& peer:app.cards) {
        if (peer.get()==&card || !peer->hwnd || IsIconic(peer->hwnd)) continue;
        RECT rect{}; if (GetWindowRect(peer->hwnd,&rect)) peers.push_back(rect);
    }
    const desk::SnapSettings settings{Dip(card,12),Dip(card,228),Dip(card,card.state.collapsed?32:190)};
    proposed=desk::SnapRect(proposed,edge,workAreas,peers,settings);
}

DWORD WINAPI WorkerMain(void* param) {
    auto* worker = static_cast<Worker*>(param);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!worker->stopping.load()) {
        WaitForSingleObject(worker->event, INFINITE);
        for (;;) {
            Job job{};
            bool found = false;
            {
                CriticalLock lock(worker->mutex);
                if (!worker->queue.empty()) {
                    job = std::move(worker->queue.front());
                    worker->queue.pop_front();
                    found = true;
                } else ResetEvent(worker->event);
            }
            if (!found || worker->stopping.load()) break;
            try {
                auto result = std::make_unique<VisualResult>();
                result->cardId = job.cardId;
                result->token = job.token;
                result->action = job.action;
                if (job.action == Open) result->okay = desk::OpenItem(nullptr, job.path);
                else if (job.action == Reveal) result->okay = desk::RevealItem(nullptr, job.path);
                else {
                    result->visual = desk::LoadShellVisual(job.path, job.size);
                    if (!job.label.empty()) result->visual.label=job.label;
                }
                if (!worker->stopping.load() && PostMessageW(app.controller, kVisual, 0,
                        reinterpret_cast<LPARAM>(result.get()))) result.release();
            } catch (...) { /* A broken shell item must not end the worker. */ }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

Worker* StartWorker() {
    auto* worker = new Worker;
    InitializeCriticalSection(&worker->mutex);
    worker->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (worker->event) worker->thread = CreateThread(nullptr, 0, WorkerMain, worker, 0, nullptr);
    if (!worker->event || !worker->thread) {
        if (worker->event) CloseHandle(worker->event);
        DeleteCriticalSection(&worker->mutex);
        delete worker;
        return nullptr;
    }
    return worker;
}

void StopWorker(Worker*& worker) {
    if (!worker) return;
    worker->stopping.store(true);
    SetEvent(worker->event);
    // A third-party icon handler can stall. Never wait indefinitely during exit.
    if (WaitForSingleObject(worker->thread, 700) == WAIT_OBJECT_0) {
        CloseHandle(worker->thread);
        CloseHandle(worker->event);
        DeleteCriticalSection(&worker->mutex);
        delete worker;
    } // On timeout the OS reclaims this worker at process exit; do not terminate it.
    worker = nullptr;
}

std::wstring FallbackLabel(const std::wstring& path) {
    size_t end = path.size();
    while (end > 3 && (path[end-1] == L'\\' || path[end-1] == L'/')) --end;
    const auto slash = path.find_last_of(L"\\/", end ? end - 1 : 0);
    std::wstring label = path.substr(slash == std::wstring::npos ? 0 : slash + 1,
        end - (slash == std::wstring::npos ? 0 : slash + 1));
    if (label.empty()) label = path;
    const auto dot = label.find_last_of(L'.');
    if (dot != std::wstring::npos &&
        (!_wcsicmp(label.c_str()+dot, L".lnk") || !_wcsicmp(label.c_str()+dot, L".url"))) label.resize(dot);
    return label;
}

void QueueVisual(Card& c, size_t index) {
    auto& item = c.items[index];
    item.token = app.nextToken++;
    if (!app.worker) return;
    const auto& reference=c.state.paths[index];
    const auto resolved=desk::ResolveDesktopItem(reference);
    Job job{c.state.id,item.token,Dip(c,c.state.layout==desk::Layout::Grid?40:16),resolved,0,
        resolved==reference?L"":FallbackLabel(reference)};
    CriticalLock lock(app.worker->mutex);
    app.worker->queue.push_back(std::move(job));
    SetEvent(app.worker->event);
}

void ReloadVisuals(Card& c) {
    if (app.worker) {
        CriticalLock lock(app.worker->mutex);
        auto& queue = app.worker->queue;
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Job& job) {
            return job.cardId == c.state.id;
        }), queue.end());
    }
    for (size_t i = 0; i < c.items.size(); ++i) QueueVisual(c, i);
}

void Save() {
    if (app.preview || app.displayChanging) return;
    desk::AppState state;
    state.nextId = app.nextId;
    state.opacity = app.opacity;
    for (auto& card : app.cards) {
        if (IsWindow(card->hwnd) && !IsIconic(card->hwnd)) desk::CaptureCardRect(card->state, card->hwnd);
        state.cards.push_back(card->state);
    }
    const bool okay = desk::SaveState(state);
    if (!okay) for (auto& card : app.cards) {
        Notify(*card,L"布局未保存：请检查配置目录权限");
    }
}

RECT ContentRect(const Card& c) {
    RECT r{}; GetClientRect(c.hwnd, &r);
    r.left += Dip(c, 10); r.right -= Dip(c, 15);
    r.top += Dip(c, 38); r.bottom -= Dip(c, 8);
    if (c.state.collapsed) r.top=r.bottom;
    return r;
}

int Columns(const Card& c) { return std::max(1, Width(ContentRect(c)) / Dip(c, 88)); }
int RowHeight(const Card& c) { return Dip(c, c.state.layout == desk::Layout::Grid ? 94 : 24); }
int ContentHeight(const Card& c) {
    const int count = static_cast<int>(c.items.size());
    return ((c.state.layout == desk::Layout::Grid ? (count + Columns(c)-1) / Columns(c) : count)) * RowHeight(c);
}
int MaxScroll(const Card& c) { return std::max(0, ContentHeight(c) - Height(ContentRect(c))); }
void ClampScroll(Card& c) { c.scroll = std::max(0, std::min(c.scroll, MaxScroll(c))); }

RECT ItemRect(const Card& c, int index) {
    RECT r = ContentRect(c);
    int columns = c.state.layout == desk::Layout::Grid ? Columns(c) : 1;
    const int cellWidth = Width(r) / columns;
    const int row = index / columns, col = index % columns;
    r.left += col * cellWidth;
    r.right = r.left + cellWidth;
    r.top += row * RowHeight(c) - c.scroll;
    r.bottom = r.top + RowHeight(c);
    return r;
}

int HitItem(const Card& c, POINT p) {
    const RECT r = ContentRect(c);
    if (!Inside(r, p)) return -1;
    const int cols = c.state.layout == desk::Layout::Grid ? Columns(c) : 1;
    const int cell = Width(r) / cols;
    const int col = (p.x - r.left) / std::max(1, cell);
    if (col >= cols) return -1;
    const int index = ((p.y - r.top + c.scroll) / RowHeight(c)) * cols + col;
    return index >= 0 && index < static_cast<int>(c.items.size()) ? index : -1;
}

int InsertionIndex(const Card& c,POINT p) {
    if(c.state.collapsed)return static_cast<int>(c.items.size());
    const RECT area=ContentRect(c);
    if(p.y<area.top)return 0;
    const int row=std::max<int>(0,p.y-area.top+c.scroll)/RowHeight(c);
    int index=0;
    if(c.state.layout==desk::Layout::List)
        index=row+((p.y-area.top+c.scroll)%RowHeight(c)>=RowHeight(c)/2?1:0);
    else {
        const int columns=Columns(c),width=std::max(1,Width(area)/columns);
        const int x=std::max(0,std::min<int>(p.x-area.left,columns*width-1));
        index=row*columns+x/width+(x%width>=width/2?1:0);
    }
    return std::max(0,std::min(index,static_cast<int>(c.items.size())));
}

RECT MenuRect(const Card& c) {
    RECT r{}; GetClientRect(c.hwnd, &r);
    r.left = r.right - Dip(c, 32); r.right -= Dip(c, 5);
    r.top = Dip(c, 3); r.bottom = Dip(c, 29); return r;
}
RECT LockRect(const Card& c) {
    RECT r = MenuRect(c); OffsetRect(&r, -Dip(c, 28), 0); return r;
}
RECT CollapseRect(const Card& c) {
    RECT r=LockRect(c); OffsetRect(&r,-Dip(c,28),0); return r;
}
RECT TitleRect(const Card& c) {
    return {Dip(c,24),Dip(c,2),CollapseRect(c).left-Dip(c,3),Dip(c,30)};
}
RECT TitleTextRect(const Card& c) {
    RECT rect=TitleRect(c);
    HDC dc=GetDC(c.hwnd);
    if (dc) {
        HGDIOBJ previous=SelectObject(dc,c.titleFont);
        TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
        SelectObject(dc,previous); ReleaseDC(c.hwnd,dc);
        rect.top+=(Height(rect)-metrics.tmHeight)/2;
        rect.bottom=rect.top+metrics.tmHeight;
    }
    return rect;
}
int HeaderHit(const Card& c, POINT point) {
    if (Inside(LockRect(c),point)) return 1;
    if (Inside(MenuRect(c),point)) return 2;
    if (Inside(CollapseRect(c),point)) return 3;
    return 0;
}

void StartFeedback(Card& c, bool success, POINT point) {
    c.feedbackActive=true; c.feedbackSuccess=success;
    c.feedbackStart=GetTickCount64(); c.dropPoint=point;
    SetTimer(c.hwnd,kAnimationTimer,16,nullptr); Invalidate(c);
}

void Notify(Card& c, const std::wstring& message) {
    c.status=message;
    c.statusUntil=message.empty()?0:GetTickCount64()+3000;
    if (message.empty()) KillTimer(c.hwnd,kStatusTimer);
    else SetTimer(c.hwnd,kStatusTimer,3000,nullptr);
    Invalidate(c);
}
RECT ScrollTrack(const Card& c) {
    RECT r{}; GetClientRect(c.hwnd, &r);
    r.left = r.right - Dip(c, 9); r.right -= Dip(c, 4);
    r.top = ContentRect(c).top; r.bottom = ContentRect(c).bottom; return r;
}
RECT ScrollThumb(const Card& c) {
    RECT r = ScrollTrack(c);
    const int height = Height(r), total = std::max(1, ContentHeight(c));
    const int thumb = std::min(height, std::max(Dip(c, 24), height * height / total));
    r.top += MaxScroll(c) ? (height - thumb) * c.scroll / MaxScroll(c) : 0;
    r.bottom = r.top + thumb; return r;
}

void Fill(HDC dc, RECT r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &r, brush); DeleteObject(brush);
}
void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width=1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
    SelectObject(dc, old); DeleteObject(pen);
}
void Text(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font); SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

COLORREF Blend(COLORREF from, COLORREF to, int percent) {
    percent=std::max(0,std::min(100,percent));
    return RGB((GetRValue(from)*(100-percent)+GetRValue(to)*percent)/100,
        (GetGValue(from)*(100-percent)+GetGValue(to)*percent)/100,
        (GetBValue(from)*(100-percent)+GetBValue(to)*percent)/100);
}

void DrawCardForeground(Card& c,HDC dc,const RECT& client,ULONGLONG now) {
    const int width = Width(client), height = Height(client);
    if (width<=0||height<=0) return;
    SetBkMode(dc,TRANSPARENT);
    const int feedback=static_cast<int>(std::min<ULONGLONG>(kFeedbackDuration,now-c.feedbackStart));
    const COLORREF feedbackColor=c.feedbackSuccess?kAccent:RGB(239,151,123);
    RECT edge = client;
    COLORREF border=c.dropHovered?kAccent:kLine;
    if (c.feedbackActive) border=Blend(kLine,feedbackColor,100-feedback*100/static_cast<int>(kFeedbackDuration));
    SetDCBrushColor(dc, border); FrameRect(dc, &edge, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int pad = Dip(c, 12);
    RECT dot{pad, Dip(c, 14), pad + Dip(c, 4), Dip(c, 18)};
    HBRUSH accent = CreateSolidBrush(kAccent);
    HGDIOBJ oldBrush = SelectObject(dc, accent), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(accent);
    RECT title=TitleTextRect(c);
    std::wstring titleValue=c.state.title;
    DWORD selectionStart=0,selectionEnd=0;
    if (c.editor) {
        wchar_t text[81]{}; GetWindowTextW(c.editor,text,81); titleValue=text;
        SendMessageW(c.editor,EM_GETSEL,reinterpret_cast<WPARAM>(&selectionStart),reinterpret_cast<LPARAM>(&selectionEnd));
    }
    if (!c.editor) Text(dc,titleValue,title,c.titleFont,kText,DT_SINGLELINE|DT_TOP|DT_END_ELLIPSIS);
    else {
        HGDIOBJ oldFont=SelectObject(dc,c.titleFont);
        SIZE start{},end{};
        GetTextExtentPoint32W(dc,titleValue.c_str(),std::min<int>(selectionStart,static_cast<int>(titleValue.size())),&start);
        GetTextExtentPoint32W(dc,titleValue.c_str(),std::min<int>(selectionEnd,static_cast<int>(titleValue.size())),&end);
        const int scroll=std::max<int>(0,end.cx-Width(title)+Dip(c,2));
        SaveDC(dc); IntersectClipRect(dc,title.left,title.top,title.right,title.bottom);
        RECT shifted=title; shifted.left-=scroll;
        Text(dc,titleValue,shifted,c.titleFont,kText,DT_SINGLELINE|DT_TOP);
        if (selectionStart!=selectionEnd) {
            RECT selected{title.left+start.cx-scroll,title.top,title.left+end.cx-scroll,title.bottom};
            Fill(dc,selected,RGB(48,83,78));
            IntersectClipRect(dc,selected.left,selected.top,selected.right,selected.bottom);
            RECT shifted=title; shifted.left-=scroll;
            Text(dc,titleValue,shifted,c.titleFont,kText,DT_SINGLELINE|DT_TOP);
        } else if (c.caretVisible) {
            Line(dc,title.left+end.cx-scroll,title.top+1,title.left+end.cx-scroll,title.bottom-1,kText);
        }
        RestoreDC(dc,-1); SelectObject(dc,oldFont);
    }
    for (int button=1;button<=3;++button) {
        const RECT bounds=button==1?LockRect(c):button==2?MenuRect(c):CollapseRect(c);
        if (c.headerPressed==button&&c.headerHover==button) {
            Fill(dc,bounds,RGB(48,83,78));
            SetDCBrushColor(dc,kAccent); FrameRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        } else if (c.headerHover==button) Fill(dc,bounds,RGB(39,57,62));
    }
    // Lock and overflow are drawn as vector strokes, independent of icon fonts.
    RECT lock = LockRect(c); int lx=(lock.left+lock.right)/2, ly=(lock.top+lock.bottom)/2;
    COLORREF lockColor = c.state.locked||c.headerHover==1||c.headerPressed==1 ? kAccent : kMuted;
    RECT body{lx-Dip(c, 5), ly-Dip(c, 1), lx+Dip(c, 5), ly+Dip(c, 7)};
    SetDCBrushColor(dc, lockColor); FrameRect(dc, &body, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    Line(dc, lx-Dip(c, 3), ly-Dip(c, 1), lx-Dip(c, 3), ly-Dip(c, 6), lockColor);
    Line(dc, lx-Dip(c, 3), ly-Dip(c, 6), lx+Dip(c, 3), ly-Dip(c, 6), lockColor);
    Line(dc, lx+Dip(c, 3), ly-Dip(c, 6), lx+Dip(c, 3), ly-Dip(c, c.state.locked?1:4), lockColor);
    RECT menu = MenuRect(c); int mx=(menu.left+menu.right)/2, my=(menu.top+menu.bottom)/2;
    for (int d=-1; d<=1; ++d) Fill(dc, {mx+Dip(c, d*5)-1,my-1,mx+Dip(c, d*5)+1,my+1}, c.headerHover==2||c.headerPressed==2?kAccent:kMuted);
    const RECT collapse=CollapseRect(c);
    const int cx=(collapse.left+collapse.right)/2,cy=(collapse.top+collapse.bottom)/2;
    const int direction=c.state.collapsed?1:-1;
    const COLORREF arrow=c.headerHover==3||c.headerPressed==3?kAccent:kMuted;
    Line(dc,cx-Dip(c,4),cy-direction*Dip(c,2),cx,cy+direction*Dip(c,2),arrow);
    Line(dc,cx,cy+direction*Dip(c,2),cx+Dip(c,4),cy-direction*Dip(c,2),arrow);
    Line(dc, Dip(c, 10), Dip(c, 31), width-Dip(c, 10), Dip(c, 31), kLine);
    if (c.state.collapsed) return;

    RECT content = ContentRect(c);
    SaveDC(dc); IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
    if (c.dropHovered) Fill(dc,content,Blend(kBg,kAccent,7));
    if (c.items.empty()) {
        RECT hint=content;
        Text(dc,c.dropHovered?L"松开以添加":L"拖入文件或快捷方式",hint,c.font,c.dropHovered?kAccent:kMuted,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    } else {
        const int cols = c.state.layout == desk::Layout::Grid ? Columns(c) : 1;
        const int first = std::max(0, c.scroll / RowHeight(c) * cols);
        const int last = std::min(static_cast<int>(c.items.size()), first + (Height(content)/RowHeight(c)+2)*cols);
        for (int i=first; i<last; ++i) {
            RECT cell = ItemRect(c, i), inner=cell;
            const bool entering=c.feedbackActive&&c.feedbackSuccess&&c.lastAddedToken&&c.items[i].token>=c.lastAddedToken;
            if (entering) OffsetRect(&cell,0,Dip(c,4)*(static_cast<int>(kFeedbackDuration)-feedback)/static_cast<int>(kFeedbackDuration));
            inner=cell;
            InflateRect(&inner, -Dip(c, 2), -Dip(c, 2));
            if (i == c.selected || i == c.hovered) Fill(dc, inner, i==c.selected ? RGB(40, 65, 65) : kHover);
            if (i==c.mouseDownItem || (c.draggingOut&&i==c.selected)) Fill(dc,inner,RGB(45,77,74));
            const auto& item = c.items[i];
            RECT label=cell;
            int ix, iy, iconSize;
            if (c.state.layout == desk::Layout::Grid) {
                iconSize = Dip(c, 38); ix=(cell.left+cell.right-iconSize)/2; iy=cell.top+Dip(c, 9);
                label.left+=Dip(c, 3); label.right-=Dip(c, 3);
                label.top = cell.top+Dip(c, 52); label.bottom = cell.bottom-Dip(c, 3);
            } else {
                iconSize=Dip(c, 16); ix=cell.left+Dip(c, 7); iy=cell.top+(Height(cell)-iconSize)/2;
                label.left=ix+iconSize+Dip(c, 8); label.right-=Dip(c, 6);
            }
            if (entering) {
                const int inset=std::max(0,iconSize*(static_cast<int>(kFeedbackDuration)-feedback)/(20*static_cast<int>(kFeedbackDuration)));
                ix+=inset; iy+=inset; iconSize-=inset*2;
            }
            if (item.icon) DrawIconEx(dc, ix, iy, item.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            else {
                RECT placeholder{ix+Dip(c, 3),iy+Dip(c, 2),ix+iconSize-Dip(c, 3),iy+iconSize-Dip(c, 2)};
                Fill(dc, placeholder, RGB(56, 69, 81));
                Line(dc, placeholder.left+Dip(c, 3), placeholder.top+Dip(c, 5), placeholder.right-Dip(c, 3), placeholder.top+Dip(c, 5), kMuted);
            }
            Text(dc, item.label, label, c.font, kText, c.state.layout==desk::Layout::Grid
                ? DT_CENTER|DT_WORDBREAK|DT_EDITCONTROL|DT_END_ELLIPSIS : DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        }
    }
    if(c.dropHovered&&app.dragSourceId==c.state.id&&!c.items.empty()) {
        const int index=InsertionIndex(c,c.dropPoint);
        RECT mark=ItemRect(c,index);
        if(c.state.layout==desk::Layout::List)
            Fill(dc,{content.left,mark.top-Dip(c,1),content.right,mark.top+Dip(c,1)},kAccent);
        else {
            if(index==static_cast<int>(c.items.size())&&index%Columns(c)) {
                mark=ItemRect(c,index-1);mark.left=mark.right;
            }
            Fill(dc,{mark.left-Dip(c,1),mark.top+Dip(c,3),mark.left+Dip(c,1),mark.bottom-Dip(c,3)},kAccent);
        }
    }
    RestoreDC(dc, -1);
    if (MaxScroll(c)>0) Fill(dc, ScrollThumb(c), RGB(79, 90, 103));
    if (!c.status.empty()&&now<c.statusUntil) {
        RECT notice{content.left,content.bottom-Dip(c,28),content.right,content.bottom};
        Fill(dc,notice,RGB(48,37,31)); InflateRect(&notice,-Dip(c,7),0);
        Text(dc,c.status,notice,c.smallFont,RGB(245,186,105),DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
}

BYTE BackgroundAlpha() { return app.opacity ? app.opacity : BYTE{96}; }

void SyncBackdrop(Card& c,int visible=-1) {
    if (!c.hwnd||!c.glass.Window()) return;
    RECT rect{};
    if (GetWindowRect(c.hwnd,&rect))
        c.glass.Sync(rect,visible<0?IsWindowVisible(c.hwnd)!=FALSE:visible!=0);
}

bool RenderCardSurface(Card& c,desk::AlphaSurface& surface) {
    RECT client{}; GetClientRect(c.hwnd,&client);
    if (!surface.Create(Width(client),Height(client))) return false;
    const ULONGLONG now=GetTickCount64();
    DrawCardForeground(c,surface.BlackDC(),client,now);
    DrawCardForeground(c,surface.WhiteDC(),client,now);
    // Only the background is tinted; DWM provides the blur on a separate surface.
    return surface.Compose(kBg,BackgroundAlpha());
}

void Paint(Card& c,HDC renderTarget=nullptr) {
    PAINTSTRUCT paint{};
    if (!renderTarget) BeginPaint(c.hwnd,&paint);
    desk::AlphaSurface surface;
    if (RenderCardSurface(c,surface)) {
        if (renderTarget) {
            RECT client{}; GetClientRect(c.hwnd,&client); Fill(renderTarget,client,kBg);
            surface.BlitOver(renderTarget);
        } else {
            RECT position{}; GetWindowRect(c.hwnd,&position);
            surface.Present(c.hwnd,{position.left,position.top});
        }
    }
    if (!renderTarget) EndPaint(c.hwnd,&paint);
}

void AddPaths(Card& c, const std::vector<std::wstring>& paths) {
    size_t added=0, unsupported=0;
    bool manageFailed=false;
    const uint64_t firstNewToken=app.nextToken;
    size_t total=0; for (auto& card:app.cards) total+=card->state.paths.size();
    const size_t count=std::min(paths.size(), c.items.size()<kMaxItems?kMaxItems-c.items.size():size_t{0});
    c.state.paths.reserve(c.state.paths.size()+count);
    c.items.reserve(c.items.size()+count);
    for (const auto& incoming : paths) {
        const auto path=desk::ReferenceForDesktopItem(incoming);
        if (!desk::IsSupportedPath(path)) { ++unsupported; continue; }
        if (c.state.paths.size() >= kMaxItems || total >= kMaxTotalItems) break;
        const auto existing=std::find_if(c.state.paths.begin(), c.state.paths.end(), [&](const std::wstring& existing) {
            return !_wcsicmp(existing.c_str(), path.c_str());
        });
        if(existing!=c.state.paths.end()) {
            if(!app.preview&&!desk::IsManagedDesktopItem(path)) {
                const auto result=ManageDesktopPath(path);
                manageFailed|=result==desk::DesktopManageResult::Failed||result==desk::DesktopManageResult::RequiresPermission;
                QueueVisual(c,static_cast<size_t>(existing-c.state.paths.begin()));
            }
            continue;
        }
        Item item{nullptr, FallbackLabel(path), 0};
        c.state.paths.push_back(path);
        c.items.push_back(std::move(item));
        ++added; ++total; Dirty();
        if(!app.preview) {
            const auto result=ManageDesktopPath(path);
            manageFailed|=result==desk::DesktopManageResult::Failed||result==desk::DesktopManageResult::RequiresPermission;
        }
        QueueVisual(c, c.items.size()-1);
    }
    Notify(c,c.state.paths.size() >= kMaxItems ? L"此卡片已达到 2048 项上限" :
        (total>=kMaxTotalItems ? L"所有卡片共支持 20000 项" : manageFailed?L"桌面项目未能隐藏，原文件仍在原处，请检查权限":unsupported ? L"部分路径不受支持" : L""));
    if (added) {
        c.lastAddedToken=firstNewToken;
        RECT area=ContentRect(c); StartFeedback(c,true,{(area.left+area.right)/2,(area.top+area.bottom)/2});
        Dirty();
    }
    Invalidate(c);
}

void EnsureSelectedVisible(Card& c) {
    if (c.selected<0) return;
    RECT r=ItemRect(c, c.selected), area=ContentRect(c);
    if (r.top<area.top) c.scroll-=area.top-r.top;
    else if (r.bottom>area.bottom) c.scroll+=r.bottom-area.bottom;
    ClampScroll(c);
}

void ShellAction(Card& c, UINT action) {
    if (c.selected<0 || c.selected>=static_cast<int>(c.state.paths.size())) return;
    if (!app.actions) app.actions=StartWorker();
    if (!app.actions) { Notify(c,L"暂时无法打开文件"); return; }
    {
        CriticalLock lock(app.actions->mutex);
        if (app.actions->queue.size()>=16) { Notify(c,L"请等待前面的文件操作完成"); return; }
        app.actions->queue.push_back({c.state.id,0,0,desk::ResolveDesktopItem(c.state.paths[c.selected]),action,{}});
        SetEvent(app.actions->event);
    }
    Notify(c,L"");
}
void OpenSelected(Card& c) { ShellAction(c,Open); }

void FinishRename(Card& c, bool accept) {
    if (!c.editor) return;
    HWND edit=c.editor; c.editor=nullptr;
    KillTimer(c.hwnd,kCaretTimer);
    if (accept) {
        wchar_t text[81]{}; GetWindowTextW(edit, text, 81);
        std::wstring title=text;
        const auto first=title.find_first_not_of(L" \t\r\n");
        const auto last=title.find_last_not_of(L" \t\r\n");
        if (first!=std::wstring::npos) {
            c.state.title=title.substr(first,last-first+1); SetWindowTextW(c.hwnd,c.state.title.c_str()); Dirty();
        }
    }
    DestroyWindow(edit); Invalidate(c);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& c=*reinterpret_cast<Card*>(data);
    if (message==WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (message==WM_PAINT) { ValidateRect(hwnd,nullptr); return 0; }
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_KEYDOWN && (wp==VK_RETURN || wp==VK_ESCAPE)) {
        FinishRename(c,wp==VK_RETURN); SetFocus(c.hwnd); return 0;
    }
    if (message==WM_KILLFOCUS) { FinishRename(c,true); return 0; }
    const LRESULT result=DefSubclassProc(hwnd,message,wp,lp);
    if (message==WM_SETFOCUS) HideCaret(hwnd);
    if (message==WM_CHAR||message==WM_KEYDOWN||message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_MOUSEMOVE||message==WM_IME_COMPOSITION) {
        c.caretVisible=true; Invalidate(c);
    }
    return result;
}

void BeginRename(Card& c) {
    if (c.editor) { SetFocus(c.editor); return; }
    RECT title=TitleTextRect(c);
    c.editor=CreateWindowExW(0,L"EDIT",c.state.title.c_str(),WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        title.left,title.top,Width(title),Height(title),c.hwnd,nullptr,app.instance,nullptr);
    if (!c.editor) return;
    SendMessageW(c.editor,EM_SETLIMITTEXT,80,0);
    SendMessageW(c.editor,WM_SETFONT,reinterpret_cast<WPARAM>(c.titleFont),TRUE);
    SendMessageW(c.editor,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
    RECT editing{0,0,Width(title),Height(title)}; SendMessageW(c.editor,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&editing));
    SetWindowSubclass(c.editor,EditProc,1,reinterpret_cast<DWORD_PTR>(&c));
    // Ordinary card clicks never activate the app. Explicit text editing needs
    // keyboard focus; its WINDOWPOS guard still keeps the card below apps.
    FocusCard(c,c.editor); HideCaret(c.editor); SendMessageW(c.editor,EM_SETSEL,0,-1);
    c.caretVisible=true; SetTimer(c.hwnd,kCaretTimer,std::max<UINT>(200,GetCaretBlinkTime()),nullptr); Invalidate(c);
}

void ToggleCollapse(Card& c) {
    FinishRename(c,true);
    desk::CaptureCardRect(c.state,c.hwnd);
    c.state.collapsed=!c.state.collapsed;
    RECT rect=desk::ResolveCardRect(c.state,desk::EnumerateMonitors());
    c.placing=true; PlaceAtDesktop(c.hwnd,&rect); c.placing=false;
    Invalidate(c); UpdateWindow(c.hwnd); Dirty();
}

Card* CreateCard(desk::CardState state);
void HandleCommand(Card* card, UINT command);

size_t ReferenceCount(const std::wstring& reference,const Card* excluded=nullptr) {
    size_t count=0;
    for (auto& card:app.cards) if (card.get()!=excluded)
        for (const auto& path:card->state.paths) if (!_wcsicmp(path.c_str(),reference.c_str())) ++count;
    return count;
}

void Append(HMENU menu, UINT command, const wchar_t* text, bool checked=false, bool enabled=true) {
    AppendMenuW(menu,MF_STRING|(checked?MF_CHECKED:0)|(enabled?0:MF_GRAYED),command,text);
}
void Separator(HMENU menu) { AppendMenuW(menu,MF_SEPARATOR,0,nullptr); }

HMENU BuildMenu(Card* c, bool itemMenu=false) {
    HMENU menu=CreatePopupMenu();
    if (itemMenu && c && c->selected>=0) {
        Append(menu,Open,L"打开\tEnter");
        Append(menu,Reveal,L"在文件夹中显示");
        Separator(menu);
        Append(menu,RemoveItem,L"从卡片移除引用\tDelete");
    } else if (c) {
            Append(menu,AddFiles,L"添加文件 / 快捷方式…");
            Append(menu,AddFolder,L"添加文件夹…");
            Append(menu,Rename,L"重命名卡片…"); Separator(menu);
            Append(menu,Grid,L"网格视图",c->state.layout==desk::Layout::Grid);
            Append(menu,List,L"列表视图",c->state.layout==desk::Layout::List);
            Append(menu,Lock,L"锁定位置和大小",c->state.locked); Separator(menu);
            Append(menu,NewCard,L"新建卡片",false,app.cards.size()<kMaxCards);
            Append(menu,RemoveCard,L"移除此卡片",false,app.cards.size()>1);
    } else {
        Append(menu,ShowCards,L"显示所有卡片");
        Append(menu,NewCard,L"新建卡片",false,app.cards.size()<kMaxCards);
        Separator(menu);
        HMENU opacity=CreatePopupMenu();
        Append(opacity,OpacityTransparent,L"轻毛玻璃 · 默认",app.opacity==0);
        Append(opacity,Opacity25,L"25%",app.opacity==64);
        Append(opacity,Opacity50,L"50%",app.opacity==128);
        Append(opacity,OpacitySolid,L"100% · 不透明",app.opacity==255);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(opacity),L"背景不透明度");
        Append(menu,Autostart,L"开机启动",desk::IsAutostartEnabled(),!app.preview);
        Append(menu,DesktopMenu,L"加入桌面右键菜单",desk::IsDesktopMenuRegistered(),!app.preview);
        Append(menu,DesktopPermission,L"授权桌面文件操作…",false,!app.preview);
        Separator(menu); Append(menu,Quit,L"退出薄荷桌面助手");
    }
    return menu;
}

void Menu(Card* c, POINT point, bool itemMenu=false) {
    ModalScope modal;
    HMENU menu=BuildMenu(c,itemMenu);
    HWND owner=c?c->hwnd:app.controller;
    if (c&&!itemMenu) { c->headerPressed=2; c->headerHover=2; Invalidate(*c); UpdateWindow(owner); }
    SetForegroundWindow(owner);
    UINT result=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,owner,nullptr);
    if (c) { c->headerPressed=0; Invalidate(*c); }
    DestroyMenu(menu); PostMessageW(owner,WM_NULL,0,0);
    if (result && !app.pendingQuit) HandleCommand(c,result);
}

void SetOpacity() {
    for (auto& c : app.cards) {
        LONG_PTR ex=GetWindowLongPtrW(c->hwnd,GWL_EXSTYLE);
        SetWindowLongPtrW(c->hwnd,GWL_EXSTYLE,ex|WS_EX_LAYERED);
        c->glass.Apply(app.opacity!=255); SyncBackdrop(*c);
        Invalidate(*c); UpdateWindow(c->hwnd);
    }
}

void NewCardAtCursor() {
    if (app.cards.size()>=kMaxCards) return;
    auto monitors=desk::EnumerateMonitors(); if (monitors.empty()) return;
    POINT cursor{}; GetCursorPos(&cursor);
    const desk::MonitorInfo* target=&monitors[0];
    for (auto& monitor:monitors) if (Inside(monitor.work,cursor)) { target=&monitor; break; }
    desk::CardState state;
    state.id=app.nextId++; state.title=L"新卡片"; state.monitor=target->device;
    state.width=340; state.height=420;
    state.x=MulDiv(cursor.x-target->work.left,96,target->dpi)-28;
    state.y=MulDiv(cursor.y-target->work.top,96,target->dpi)-24;
    if (auto* card=CreateCard(std::move(state))) {
        SetOpacity(); ShowWindow(card->hwnd,SW_SHOWNOACTIVATE); Invalidate(*card);
    }
    Dirty();
}

void HandleCommand(Card* c, UINT command) {
    switch (command) {
    case Quit: PostMessageW(app.controller,WM_CLOSE,0,0); return;
    case NewCard: NewCardAtCursor(); return;
    case ShowCards:
        for (auto& card:app.cards) { ShowWindow(card->hwnd,SW_SHOWNOACTIVATE); PlaceAtDesktop(card->hwnd); }
        return;
    case Autostart: {
        if (app.preview) return;
        const bool enable=!desk::IsAutostartEnabled();
        const bool okay=desk::SetAutostart(enable);
        for (auto& card:app.cards) Notify(*card,okay?L"":L"无法更新开机启动设置");
        return;
    }
    case DesktopMenu: {
        if (app.preview) return;
        const bool enable=!desk::IsDesktopMenuRegistered();
        const bool okay=desk::SetDesktopMenu(enable);
        for (auto& card:app.cards) {
            Notify(*card,okay?L"":L"无法更新桌面右键菜单");
        }
        return;
    }
    case DesktopPermission: {
        if(app.preview)return;
        ModalScope modal;
        const bool granted=desk::EnsureFileHelper(app.controller,true);
        if(granted&&!app.pendingQuit) {
            app.desktopReady=RestoreDesktopWithPermission();
            for(auto& card:app.cards) {
                bool okay=app.desktopReady;
                if(app.desktopReady)for(const auto& path:card->state.paths) {
                    const auto result=ManageDesktopPath(path);
                    okay&=result!=desk::DesktopManageResult::Failed&&result!=desk::DesktopManageResult::RequiresPermission;
                }
                ReloadVisuals(*card);
                Notify(*card,okay?L"":L"部分桌面项目暂时无法隐藏，请检查权限");
            }
        }
        return;
    }
    case OpacitySolid: case Opacity50: case Opacity25: case OpacityTransparent:
        app.opacity=command==OpacitySolid?255:command==Opacity50?128:command==Opacity25?64:0;
        SetOpacity(); Dirty(); return;
    default: break;
    }
    if (!c) return;
    c->status.clear();
    switch (command) {
    case AddFiles: case AddFolder: {
        ModalScope modal;
        auto paths=desk::PickItems(c->hwnd,command==AddFolder);
        if (!app.pendingQuit) AddPaths(*c,paths);
        break;
    }
    case Rename: BeginRename(*c); break;
    case Grid: case List:
        c->state.layout=command==Grid?desk::Layout::Grid:desk::Layout::List;
        c->scroll=0; ReloadVisuals(*c); EnsureSelectedVisible(*c); Dirty(); break;
    case Lock: c->state.locked=!c->state.locked; Dirty(); break;
    case RemoveCard:
        if (app.cards.size()>1 && app.modalDepth<=1 && !app.dragSourceId) {
            if (!app.preview) for (const auto& reference:c->state.paths) {
                if (!ReferenceCount(reference,c)&&!ReleaseDesktopWithPermission(reference)) {
                    Notify(*c,L"桌面项目暂时无法还原，请关闭占用它的程序后重试"); return;
                }
            }
            const HWND window=c->hwnd;
            DestroyWindow(window);
            app.cards.erase(std::remove_if(app.cards.begin(),app.cards.end(),[&](const std::unique_ptr<Card>& card) {return card.get()==c;}),app.cards.end());
            Dirty(); return;
        } break;
    case Open: OpenSelected(*c); break;
    case Reveal:
        ShellAction(*c,command); break;
    case RemoveItem:
        if(app.dragSourceId)break;
        if (c->selected>=0 && c->selected<static_cast<int>(c->state.paths.size())) {
            const auto& reference=c->state.paths[c->selected];
            if (!app.preview&&ReferenceCount(reference)<=1&&!ReleaseDesktopWithPermission(reference)) {
                Notify(*c,L"桌面项目暂时无法还原，请关闭占用它的程序后重试"); break;
            }
            c->mouseDownItem=-1;
            if (GetCapture()==c->hwnd) ReleaseCapture();
            if (c->items[c->selected].icon) DestroyIcon(c->items[c->selected].icon);
            c->state.paths.erase(c->state.paths.begin()+c->selected);
            c->items.erase(c->items.begin()+c->selected);
            c->selected=std::min(c->selected,static_cast<int>(c->items.size())-1); c->hovered=-1;
            ClampScroll(*c); Dirty();
        } break;
    default: break;
    }
    Invalidate(*c);
}

void ClearDragFeedback(Card& c) {
    c.mouseDownItem=-1;c.draggingOut=false;c.hovered=-1;c.dropHovered=false;
    c.headerHover=0;c.headerPressed=0;c.lastAddedToken=0;c.feedbackActive=false;
    KillTimer(c.hwnd,kAnimationTimer);
    KillTimer(c.hwnd,kDragScrollTimer);
    if(GetCapture()==c.hwnd)ReleaseCapture();
}

Card* ActiveDragSource(const std::vector<std::wstring>& paths) {
    if(!app.dragSourceId||!app.dragReference||paths.size()!=1||
        _wcsicmp(desk::ReferenceForDesktopItem(paths[0]).c_str(),app.dragReference->c_str()))return nullptr;
    for(auto& card:app.cards)if(card->state.id==app.dragSourceId)return card.get();
    return nullptr;
}

bool MoveBetweenCards(Card& source,Card& target,const std::wstring& reference,bool copy,int insertion=-1) {
    const auto from=std::find_if(source.state.paths.begin(),source.state.paths.end(),[&](const std::wstring& path){return !_wcsicmp(path.c_str(),reference.c_str());});
    if(from==source.state.paths.end())return false;
    const size_t fromIndex=static_cast<size_t>(from-source.state.paths.begin());
    if(fromIndex>=source.items.size())return false;
    if(&source==&target){
        int destination=static_cast<int>(fromIndex);
        if(insertion>=0) {
            destination=std::min(insertion,static_cast<int>(source.items.size()));
            if(destination>static_cast<int>(fromIndex))--destination;
            destination=std::max(0,destination);
            auto rotate=[&](auto& items) {
                if(destination<static_cast<int>(fromIndex))
                    std::rotate(items.begin()+destination,items.begin()+fromIndex,items.begin()+fromIndex+1);
                else if(destination>static_cast<int>(fromIndex))
                    std::rotate(items.begin()+fromIndex,items.begin()+fromIndex+1,items.begin()+destination+1);
            };
            rotate(source.state.paths);rotate(source.items);
            if(destination!=static_cast<int>(fromIndex))Dirty();
        }
        ClearDragFeedback(source);source.selected=destination;EnsureSelectedVisible(source);Invalidate(source);return true;
    }
    const auto to=std::find_if(target.state.paths.begin(),target.state.paths.end(),[&](const std::wstring& path){return !_wcsicmp(path.c_str(),reference.c_str());});
    size_t toIndex=static_cast<size_t>(to-target.state.paths.begin());
    bool refreshIcon=false;
    if(to==target.state.paths.end()) {
        size_t total=0;for(auto& card:app.cards)total+=card->state.paths.size();
        if(target.state.paths.size()>=kMaxItems||(copy&&total>=kMaxTotalItems))return false;
        target.state.paths.reserve(target.state.paths.size()+1);
        target.items.reserve(target.items.size()+1);
        // Prepare anything that could allocate before removing the source.
        Item item;
        if(copy) {
            item.label=source.items[fromIndex].label;item.token=app.nextToken++;
            if(source.items[fromIndex].icon){item.icon=CopyIcon(source.items[fromIndex].icon);if(!item.icon)return false;}
        } else item=std::move(source.items[fromIndex]);
        try { target.state.paths.push_back(reference); }
        catch(...) {if(copy&&item.icon)DestroyIcon(item.icon);else source.items[fromIndex].label=std::move(item.label);throw;}
        target.items.push_back(std::move(item));
        if(!copy)source.items[fromIndex].icon=nullptr;
        refreshIcon=source.state.layout!=target.state.layout||source.dpi!=target.dpi||!target.items.back().icon;
    } else if(toIndex>=target.items.size())return false;
    else if(!target.items[toIndex].icon&&source.items[fromIndex].icon) {
        HICON icon=copy?CopyIcon(source.items[fromIndex].icon):source.items[fromIndex].icon;
        if(!icon)return false;
        target.items[toIndex].icon=icon;
        if(!copy)source.items[fromIndex].icon=nullptr;
        refreshIcon=source.state.layout!=target.state.layout||source.dpi!=target.dpi;
    }
    if(!copy) {
        if(source.items[fromIndex].icon)DestroyIcon(source.items[fromIndex].icon);
        source.items.erase(source.items.begin()+fromIndex);
        source.state.paths.erase(source.state.paths.begin()+fromIndex);
        source.selected=-1;ClampScroll(source);
    }
    ClearDragFeedback(source);ClearDragFeedback(target);
    if(refreshIcon)try{QueueVisual(target,toIndex);}catch(...){/* Existing icon remains usable. */}
    target.selected=static_cast<int>(toIndex);
    if(target.state.collapsed)ToggleCollapse(target);
    EnsureSelectedVisible(target);
    if(IsWindowVisible(target.hwnd))FocusCard(target,target.hwnd);
    Invalidate(source);Invalidate(target);UpdateWindow(source.hwnd);UpdateWindow(target.hwnd);
    Dirty();return true;
}

bool CardMoveFeedback(void*,const std::vector<std::wstring>& paths,DWORD keys) {
    return !(keys&MK_CONTROL)&&ActiveDragSource(paths)!=nullptr;
}

void DropHover(void* context, bool hovering, POINT screenPoint) {
    auto& c=*static_cast<Card*>(context);
    POINT point=screenPoint; ScreenToClient(c.hwnd,&point);
    const bool changed=c.dropHovered!=hovering || (hovering&&(c.dropPoint.x!=point.x||c.dropPoint.y!=point.y));
    c.dropHovered=hovering; c.dropPoint=point;
    if(hovering&&app.dragSourceId)SetTimer(c.hwnd,kDragScrollTimer,80,nullptr);
    else KillTimer(c.hwnd,kDragScrollTimer);
    if (changed) Invalidate(c);
}

bool DropFiles(void* context,const std::vector<std::wstring>& paths,POINT screenPoint,DWORD keys) {
    auto& c=*static_cast<Card*>(context);
    if(app.quitting)return false;
    if (paths.empty()) return false;
    POINT point=screenPoint;ScreenToClient(c.hwnd,&point);
    if(Card* source=ActiveDragSource(paths)) {
        const bool accepted=MoveBetweenCards(*source,c,*app.dragReference,(keys&MK_CONTROL)!=0,
            source==&c?InsertionIndex(c,point):-1);
        if(accepted) {
            app.dragHandledInside=true;
            if(c.state.collapsed)ToggleCollapse(c);
        }
        StartFeedback(c,accepted,point);UpdateWindow(c.hwnd);return accepted;
    }
    if (c.state.collapsed) ToggleCollapse(c);
    const size_t previous=c.state.paths.size();
    AddPaths(c,paths);
    const bool accepted=c.state.paths.size()>previous ||
        std::all_of(paths.begin(),paths.end(),[&](const std::wstring& incoming) {
            const auto path=desk::ReferenceForDesktopItem(incoming);
            return std::any_of(c.state.paths.begin(),c.state.paths.end(),[&](const std::wstring& existing) {
                return !_wcsicmp(existing.c_str(),path.c_str());
            });
        });
    StartFeedback(c,accepted,point);UpdateWindow(c.hwnd);return accepted;
}

LRESULT CardMessage(Card& c, HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        c.hwnd=hwnd; c.dpi=WindowDpi(hwnd); Fonts(c);
        c.oleDropRegistered=desk::RegisterFileDrop(hwnd,{&c,DropHover,DropFiles,CardMoveFeedback});
        if (!c.oleDropRegistered) DragAcceptFiles(hwnd,TRUE);
        { BOOL disabled=TRUE; DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&disabled,sizeof(disabled)); }
        c.glass.Create(hwnd); c.glass.Apply(app.opacity!=255);
        return 0;
    case WM_NCCALCSIZE: return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(c); return 0;
    case WM_WINDOWPOSCHANGED: EnforceDesktopBand(c); SyncBackdrop(c); break;
    case WM_SHOWWINDOW: SyncBackdrop(c,wp?1:0); break;
    case WM_DWMCOMPOSITIONCHANGED: case WM_THEMECHANGED:
        c.glass.Apply(app.opacity!=255); SyncBackdrop(c); Invalidate(c); break;
    case WM_NCACTIVATE: Invalidate(c); return TRUE;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: {
        auto* position=reinterpret_cast<WINDOWPOS*>(lp);
        if (position&&!(position->flags&SWP_NOZORDER)) {
            HWND anchor=nullptr;
            if (DesktopAnchor(anchor)) position->hwndInsertAfter=anchor;
            else position->flags|=SWP_NOZORDER;
            position->flags|=SWP_NOACTIVATE;
        }
        // Keep DefWindowProc's native min/max handling during move/resize.
        break;
    }
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(hwnd,&p);
        RECT r{}; GetClientRect(hwnd,&r); const int edge=Dip(c,6);
        if (HeaderHit(c,p)) return HTCLIENT;
        if (!c.state.locked) {
            const bool left=p.x<edge, right=p.x>=r.right-edge, top=p.y<edge, bottom=p.y>=r.bottom-edge;
            if (top&&left&&!c.state.collapsed) return HTTOPLEFT;
            if (top&&right&&!c.state.collapsed) return HTTOPRIGHT;
            if (bottom&&left&&!c.state.collapsed) return HTBOTTOMLEFT;
            if (bottom&&right&&!c.state.collapsed) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top&&!c.state.collapsed) return HTTOP;
            if (bottom&&!c.state.collapsed) return HTBOTTOM;
            if (p.y<Dip(c,32) && !c.editor) return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize={Dip(c,228),Dip(c,c.state.collapsed?32:190)};
        return 0;
    }
    case WM_NCLBUTTONDBLCLK: if (wp==HTCAPTION) { BeginRename(c); return 0; } break;
    case WM_NCRBUTTONUP: case WM_CONTEXTMENU: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (p.x==-1 && p.y==-1) { p={Dip(c,20),Dip(c,38)}; ClientToScreen(hwnd,&p); }
        POINT local=p; ScreenToClient(hwnd,&local);
        const int index=HitItem(c,local);
        if (index>=0) { c.selected=index; Invalidate(c); }
        Menu(&c,p,index>=0); return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (c.editor) FinishRename(c,true);
        const int button=HeaderHit(c,p);
        c.headerHover=button; c.headerPressed=button; c.mouseDownItem=-1;
        if (button) { SetCapture(hwnd); Invalidate(c); return 0; }
        FocusCard(c,hwnd);
        if (MaxScroll(c)>0 && Inside(ScrollTrack(c),p)) {
            RECT thumb=ScrollThumb(c);
            if (!Inside(thumb,p)) {
                c.scroll+=p.y<thumb.top?-Height(ContentRect(c)):Height(ContentRect(c)); ClampScroll(c); Invalidate(c);
            } else { c.draggingScroll=true; c.scrollGrab=p.y-thumb.top; SetCapture(hwnd); }
            return 0;
        }
        c.selected=HitItem(c,p); c.mouseDownItem=c.selected; c.mouseDownPoint=p;
        if (c.mouseDownItem>=0) SetCapture(hwnd);
        Notify(c,L""); Invalidate(c); return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (HeaderHit(c,p)) return 0;
        if (p.y<Dip(c,32)) BeginRename(c);
        else { c.selected=HitItem(c,p); OpenSelected(c); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (c.draggingScroll) {
            RECT track=ScrollTrack(c), thumb=ScrollThumb(c);
            const int distance=Height(track)-Height(thumb);
            if (distance>0) c.scroll=MulDiv(p.y-track.top-c.scrollGrab,MaxScroll(c),distance);
            ClampScroll(c); Invalidate(c); return 0;
        }
        if (c.mouseDownItem>=0&&(wp&MK_LBUTTON)&&GetCapture()==hwnd) {
            if (c.mouseDownItem>=static_cast<int>(c.state.paths.size())||c.mouseDownItem>=static_cast<int>(c.items.size())) {
                c.mouseDownItem=-1; ReleaseCapture(); return 0;
            }
            if (abs(p.x-c.mouseDownPoint.x)>=GetSystemMetrics(SM_CXDRAG)||abs(p.y-c.mouseDownPoint.y)>=GetSystemMetrics(SM_CYDRAG)) {
                const int index=c.mouseDownItem;
                const std::wstring reference=c.state.paths[index];
                const std::wstring path=desk::ResolveDesktopItem(reference);
                HICON icon=c.items[index].icon;
                if(app.dragSourceId)return 0;
                ClearDragFeedback(c);c.draggingOut=true;Invalidate(c);UpdateWindow(hwnd);
                ModalScope modal;
                CardDragScope cardDrag(c,reference);
                ExternalVisibilityScope visibility{path};
                bool desktopDrop=false;
                const DWORD effect=desk::DragFileReference(hwnd,path,icon,desk::IsManagedDesktopItem(reference)?&desktopDrop:nullptr,
                    ExternalVisibilityScope::Prepare,&visibility);
                const bool handledInside=app.dragHandledInside;
                cardDrag.Finish();
                ClearDragFeedback(c);
                if (desktopDrop) {
                    if (!ReleaseDesktopWithPermission(reference)) {
                        Notify(c,L"桌面项目暂时无法还原，请关闭占用它的程序后重试"); return 0;
                    }
                    const auto found=std::find(c.state.paths.begin(),c.state.paths.end(),reference);
                    if (found!=c.state.paths.end()) { c.selected=static_cast<int>(found-c.state.paths.begin()); HandleCommand(&c,RemoveItem); }
                    Invalidate(c); return 0;
                }
                if (effect!=DROPEFFECT_NONE&&!handledInside) StartFeedback(c,true,p);
                Invalidate(c); return 0;
            }
        }
        const int button=HeaderHit(c,p);
        if (button!=c.headerHover) { c.headerHover=button; Invalidate(c); }
        const int index=HitItem(c,p); if (index!=c.hovered) { c.hovered=index; Invalidate(c); }
        if (!c.tracking) { TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0}; TrackMouseEvent(&t); c.tracking=true; }
        return 0;
    }
    case WM_MOUSELEAVE: c.tracking=false; c.hovered=-1; c.headerHover=0; Invalidate(c); return 0;
    case WM_LBUTTONUP: {
        const POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        const int pressed=c.headerPressed;
        c.headerPressed=0; c.mouseDownItem=-1; c.draggingScroll=false;
        if (GetCapture()==hwnd) ReleaseCapture();
        c.headerHover=HeaderHit(c,p); Invalidate(c);
        if (pressed&&pressed==c.headerHover) {
            if (pressed==1) HandleCommand(&c,Lock);
            else if (pressed==3) ToggleCollapse(c);
            else { POINT at{MenuRect(c).left,MenuRect(c).bottom}; ClientToScreen(hwnd,&at); Menu(&c,at); }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        c.draggingScroll=false; c.headerPressed=0; c.mouseDownItem=-1; Invalidate(c); return 0;
    case WM_TIMER:
        if(wp==kDragScrollTimer) {
            if(!c.dropHovered||!app.dragSourceId||c.state.collapsed)KillTimer(hwnd,kDragScrollTimer);
            else {
                POINT p{};GetCursorPos(&p);ScreenToClient(hwnd,&p);c.dropPoint=p;
                const RECT area=ContentRect(c);const int edge=Dip(c,20),previous=c.scroll;
                if(p.y<area.top+edge)c.scroll-=std::max(1,RowHeight(c)/2);
                else if(p.y>area.bottom-edge)c.scroll+=std::max(1,RowHeight(c)/2);
                ClampScroll(c);if(c.scroll!=previous)Invalidate(c);
            }
            return 0;
        }
        if (wp==kAnimationTimer) {
            if (GetTickCount64()-c.feedbackStart>=kFeedbackDuration) {
                KillTimer(hwnd,kAnimationTimer); c.feedbackActive=false; c.lastAddedToken=0;
            }
            Invalidate(c); return 0;
        }
        if (wp==kStatusTimer) { KillTimer(hwnd,kStatusTimer); c.status.clear(); c.statusUntil=0; Invalidate(c); return 0; }
        if (wp==kCaretTimer) {
            if (!c.editor) KillTimer(hwnd,kCaretTimer);
            else { c.caretVisible=!c.caretVisible; Invalidate(c); }
            return 0;
        }
        break;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp)==c.editor&&(HIWORD(wp)==EN_CHANGE||HIWORD(wp)==EN_UPDATE)) { c.caretVisible=true; Invalidate(c); return 0; }
        break;
    case WM_MOUSEWHEEL: {
        c.wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);
        const int lines=c.wheelRemainder/WHEEL_DELTA; c.wheelRemainder%=WHEEL_DELTA;
        c.scroll-=lines*RowHeight(c)*(c.state.layout==desk::Layout::Grid?1:3); ClampScroll(c); Invalidate(c); return 0;
    }
    case WM_KEYDOWN: {
        const int count=static_cast<int>(c.items.size());
        if (wp==VK_RETURN) { OpenSelected(c); return 0; }
        if (wp==VK_DELETE) { HandleCommand(&c,RemoveItem); return 0; }
        if (wp==VK_F2) { BeginRename(c); return 0; }
        if (wp==VK_APPS || (wp==VK_F10 && GetKeyState(VK_SHIFT)<0)) { SendMessageW(hwnd,WM_CONTEXTMENU,0,MAKELPARAM(-1,-1)); return 0; }
        if (wp==VK_ESCAPE) { c.selected=-1; c.status.clear(); Invalidate(c); return 0; }
        if (GetKeyState(VK_CONTROL)<0 && (wp==L'O' || wp==L'N')) { HandleCommand(&c,wp==L'O'?AddFiles:NewCard); return 0; }
        if (!count) return 0;
        int index=c.selected;
        const int cols=c.state.layout==desk::Layout::Grid?Columns(c):1;
        if (index<0 && (wp==VK_LEFT || wp==VK_RIGHT || wp==VK_UP || wp==VK_DOWN)) {
            c.selected=0; EnsureSelectedVisible(c); Invalidate(c); return 0;
        }
        if (wp==VK_HOME) index=0; else if (wp==VK_END) index=count-1;
        else if (wp==VK_LEFT) index=std::max(0,index-1); else if (wp==VK_RIGHT) index=std::min(count-1,index+1);
        else if (wp==VK_UP) index=std::max(0,index-cols); else if (wp==VK_DOWN) index=std::min(count-1,index+cols);
        else if (wp==VK_PRIOR) index=std::max(0,index-std::max(1,Height(ContentRect(c))/RowHeight(c))*cols);
        else if (wp==VK_NEXT) index=std::min(count-1,index+std::max(1,Height(ContentRect(c))/RowHeight(c))*cols);
        else return 0;
        c.selected=index; EnsureSelectedVisible(c); Invalidate(c); return 0;
    }
    case WM_DROPFILES: {
        HDROP drop=reinterpret_cast<HDROP>(wp);
        auto paths=desk::PathsFromDrop(drop); DragFinish(drop); AddPaths(c,paths); return 0;
    }
    case WM_ENTERSIZEMOVE: if (c.editor) FinishRename(c,true); return 0;
    case WM_MOVING:
        if (lp && !c.state.locked && !(GetAsyncKeyState(VK_MENU)&0x8000))
            SnapCardRect(c,*reinterpret_cast<RECT*>(lp),desk::SnapEdge::Move);
        return TRUE;
    case WM_SIZING:
        if (c.state.collapsed&&wp!=WMSZ_LEFT&&wp!=WMSZ_RIGHT) return TRUE;
        if (lp && wp>=WMSZ_LEFT && wp<=WMSZ_BOTTOMRIGHT && !c.state.locked && !(GetAsyncKeyState(VK_MENU)&0x8000))
            SnapCardRect(c,*reinterpret_cast<RECT*>(lp),static_cast<desk::SnapEdge>(wp));
        return TRUE;
    case WM_EXITSIZEMOVE:
        desk::CaptureCardRect(c.state,hwnd); ClampScroll(c); Dirty(); return 0;
    case WM_SIZE: ClampScroll(c); Invalidate(c); return 0;
    case WM_DPICHANGED: {
        FinishRename(c,true); c.dpi=HIWORD(wp); Fonts(c);
        auto* suggested=reinterpret_cast<RECT*>(lp);
        if (!c.placing && !app.displayChanging)
            SetWindowPos(hwnd,nullptr,suggested->left,suggested->top,Width(*suggested),Height(*suggested),SWP_NOZORDER|SWP_NOACTIVATE);
        ReloadVisuals(c); ClampScroll(c); Invalidate(c); Dirty(); return 0;
    }
    case WM_DISPLAYCHANGE:
        PostMessageW(app.controller,WM_DISPLAYCHANGE,wp,lp); return 0;
    case WM_SETTINGCHANGE:
        c.glass.Apply(app.opacity!=255); SyncBackdrop(c); Invalidate(c);
        if (wp==SPI_SETWORKAREA) PostMessageW(app.controller,WM_SETTINGCHANGE,wp,lp);
        return 0;
    case WM_ACTIVATE: PostMessageW(app.controller,kLower,0,0); return 0;
    case WM_SYSCOMMAND:
        if ((wp&0xfff0)==SC_CLOSE) return 0; // Alt+F4 must not silently lose a card.
        if ((wp&0xfff0)==SC_MAXIMIZE || (wp&0xfff0)==SC_MINIMIZE) return 0;
        if (c.state.locked && ((wp&0xfff0)==SC_MOVE || (wp&0xfff0)==SC_SIZE)) return 0;
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp),kText); SetBkColor(reinterpret_cast<HDC>(wp),kHover);
        SetDCBrushColor(reinterpret_cast<HDC>(wp),kHover);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_CLOSE: return 0;
    case WM_DESTROY:
        c.glass.Destroy();
        desk::RevokeFileDrop(hwnd); DragAcceptFiles(hwnd,FALSE);
        KillTimer(hwnd,kAnimationTimer); KillTimer(hwnd,kStatusTimer); KillTimer(hwnd,kCaretTimer); return 0;
    case WM_NCDESTROY: c.hwnd=nullptr; SetWindowLongPtrW(hwnd,GWLP_USERDATA,0); break;
    default: break;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

LRESULT CALLBACK CardProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* card=reinterpret_cast<Card*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        card=static_cast<Card*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(card)); card->hwnd=hwnd;
    }
    if (!card) return DefWindowProcW(hwnd,message,wp,lp);
    try { return CardMessage(*card,hwnd,message,wp,lp); }
    catch (...) {
        try { card->status=L"操作未完成，卡片仍可继续使用"; } catch (...) {}
        Invalidate(*card); return 0;
    }
}

Card* CreateCard(desk::CardState state) {
    auto card=std::make_unique<Card>(); card->state=std::move(state);
    if (!app.preview&&app.desktopReady) for (const auto& path:card->state.paths) ManageDesktopPath(path);
    // Allocate potentially throwing data before binding the HWND to this object.
    card->items.reserve(card->state.paths.size());
    for (auto& path:card->state.paths) card->items.push_back({nullptr,FallbackLabel(path),0});
    app.cards.reserve(app.cards.size()+1);
    const auto monitors=desk::EnumerateMonitors();
    RECT rect=desk::ResolveCardRect(card->state,monitors);
    if (!monitors.empty()) {
        card->dpi=monitors.front().dpi;
        for (const auto& monitor:monitors) if (monitor.device==card->state.monitor) { card->dpi=monitor.dpi; break; }
    }
    SnapCardRect(*card,rect,desk::SnapEdge::Move);
    card->placing=true;
    HWND hwnd=CreateWindowExW((app.preview?WS_EX_APPWINDOW:WS_EX_TOOLWINDOW)|WS_EX_LAYERED|WS_EX_NOACTIVATE,kCardClass,card->state.title.c_str(),WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,
        rect.left,rect.top,Width(rect),Height(rect),nullptr,nullptr,app.instance,card.get());
    card->placing=false;
    if (!hwnd) return nullptr;
    Card* result=card.get(); app.cards.push_back(std::move(card)); ReloadVisuals(*result);
    PlaceAtDesktop(hwnd);
    return result;
}

bool Tray(DWORD action) {
    NOTIFYICONDATAW data{}; data.cbSize=sizeof(data); data.hWnd=app.controller; data.uID=1;
    data.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; data.uCallbackMessage=kTray; data.hIcon=app.icon;
    lstrcpynW(data.szTip,L"薄荷桌面助手 · MintDesk",ARRAYSIZE(data.szTip));
    bool result=Shell_NotifyIconW(action,&data)!=FALSE;
    if (action==NIM_ADD && result) { data.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&data); }
    return result;
}

void LowerCards() {
    // Activation and foreground events both enforce the band, including while
    // a title editor or one of our menus temporarily owns keyboard focus.
    for (auto& c:app.cards) if (c->hwnd)
        PlaceAtDesktop(c->hwnd);
}

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (app.controller && !app.quitting) PostMessageW(app.controller,kLower,0,0);
}

void ReflowDisplays() {
    app.displayChanging=true;
    auto monitors=desk::EnumerateMonitors();
    for (auto& card:app.cards) {
        RECT rect=desk::ResolveCardRect(card->state,monitors);
        SnapCardRect(*card,rect,desk::SnapEdge::Move);
        card->placing=true; PlaceAtDesktop(card->hwnd,&rect); card->placing=false;
        desk::CaptureCardRect(card->state,card->hwnd);
    }
    app.displayChanging=false; Dirty();
}

LRESULT ControllerMessage(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message==app.taskbarCreated && !app.preview) { app.trayAdded=Tray(NIM_ADD); return 0; }
    switch (message) {
    case kVisual: {
        std::unique_ptr<VisualResult> result(reinterpret_cast<VisualResult*>(lp));
        for (auto& card:app.cards) if (card->state.id==result->cardId) {
            if (result->action) {
                Notify(*card,result->okay?L"":L"无法打开：文件可能已移动或不可用");
            } else for (auto& item:card->items) if (item.token==result->token) {
                if (item.icon) DestroyIcon(item.icon);
                item.icon=result->visual.icon; result->visual.icon=nullptr;
                if (!result->visual.label.empty()) item.label=std::move(result->visual.label);
                break;
            }
            Invalidate(*card); break;
        }
        return 0;
    }
    case kTray: {
        UINT event=LOWORD(lp);
        if (event==WM_CONTEXTMENU || event==WM_RBUTTONUP) { POINT p{}; GetCursorPos(&p); Menu(nullptr,p); }
        else if (event==NIN_SELECT || event==NIN_KEYSELECT || event==WM_LBUTTONDBLCLK) HandleCommand(nullptr,ShowCards);
        return 0;
    }
    case kLower: LowerCards(); return 0;
    case kShow: HandleCommand(nullptr,ShowCards); return 0;
    case kNewCard: HandleCommand(nullptr,NewCard); return 0;
    case WM_TIMER:
        KillTimer(hwnd,wp);
        if (wp==kSaveTimer) Save(); else if (wp==kDisplayTimer) ReflowDisplays(); return 0;
    case WM_DISPLAYCHANGE:
        app.displayChanging=true; KillTimer(hwnd,kSaveTimer); SetTimer(hwnd,kDisplayTimer,500,nullptr); return 0;
    case WM_SETTINGCHANGE:
        if (wp==SPI_SETWORKAREA) { app.displayChanging=true; KillTimer(hwnd,kSaveTimer); SetTimer(hwnd,kDisplayTimer,500,nullptr); }
        return 0;
    case WM_QUERYENDSESSION:
        if (app.displayChanging) ReflowDisplays();
        Save(); return TRUE;
    case WM_ENDSESSION:
        if (wp) { app.sessionEnding=true; SendMessageW(hwnd,WM_CLOSE,0,0); }
        return 0;
    case WM_CLOSE:
        if (app.quitting) return 0;
        if (app.modalDepth) { app.pendingQuit=true; return 0; }
        app.quitting=true; KillTimer(hwnd,kSaveTimer); KillTimer(hwnd,kDisplayTimer);
        for (auto& card:app.cards) FinishRename(*card,true);
        if (app.displayChanging) ReflowDisplays();
        StopWorker(app.worker); StopWorker(app.actions);
        if (!app.preview&&!RestoreDesktopWithPermission()) {
            app.desktopReady=false;
            if (!app.sessionEnding) {
                app.quitting=false; app.pendingQuit=false;
                Save();
                app.worker=StartWorker();
                for (auto& card:app.cards) {
                    ReloadVisuals(*card);
                    Notify(*card,L"有项目未能还原，尚未退出；请关闭占用程序后再退出");
                }
                return 0;
            }
            app.exitCode=2;
        }
        if (app.foregroundHook) { UnhookWinEvent(app.foregroundHook); app.foregroundHook=nullptr; }
        if (app.trayAdded) Tray(NIM_DELETE);
        Save();
        if (!app.preview) desk::RefreshDesktopMenu(false);
        for (auto& card:app.cards) if (card->hwnd) DestroyWindow(card->hwnd);
        { MSG pending{}; while (PeekMessageW(&pending,nullptr,kVisual,kVisual,PM_REMOVE)) {
            auto* result=reinterpret_cast<VisualResult*>(pending.lParam);
            delete result;
        } }
        app.cards.clear();
        if (!app.preview) desk::ClearDesktopItems();
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY: app.controller=nullptr; PostQuitMessage(app.exitCode); return 0;
    default: return DefWindowProcW(hwnd,message,wp,lp);
    }
}

LRESULT CALLBACK ControllerProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    try { return ControllerMessage(hwnd,message,wp,lp); }
    catch (...) { if (message==WM_CLOSE) PostQuitMessage(1); return 0; }
}

void DefaultCards() {
    auto monitors=desk::EnumerateMonitors(); if (monitors.empty()) return;
    const auto& monitor=monitors.front();
    const int available=MulDiv(Width(monitor.work),96,monitor.dpi);
    const int tall=MulDiv(Height(monitor.work),96,monitor.dpi);
    const int gap=0, margin=0;
    const int count=available>=1000?3:available>=650?2:1;
    const int width=std::min(350,(available-margin*2-gap*(count-1))/count);
    const wchar_t* titles[]={L"常用",L"项目",L"工具"};
    for (int i=0;i<count;++i) {
        desk::CardState state; state.id=app.nextId++; state.title=titles[i];
        state.monitor=monitor.device; state.x=margin+i*(width+gap); state.y=margin;
        state.width=std::max(228,width); state.height=std::max(190,std::min(480,tall-margin*2));
        state.layout=i==0?desk::Layout::Grid:desk::Layout::List;
        CreateCard(std::move(state));
    }
}

bool Initialize(HINSTANCE instance) {
    app.instance=instance;
    using DpiContext=BOOL(WINAPI*)(HANDLE);
    auto setDpi=User32Function<DpiContext>("SetProcessDpiAwarenessContext");
    if (setDpi) setDpi(reinterpret_cast<HANDLE>(-4)); else SetProcessDPIAware();
    // Preserve the production singleton identity across the product rename so
    // an old executable cannot concurrently manage the same recovery journal.
    app.singleton=CreateMutexW(nullptr,FALSE,app.preview?L"Local\\MintDesk.Preview.1":L"Local\\DeskCards.Instance.1");
    if (!app.singleton) return false;
    if (GetLastError()==ERROR_ALREADY_EXISTS) {
        HWND other=FindWindowW(kControllerClass,app.preview?L"MintDesk Preview":L"MintDesk");
        if (other) PostMessageW(other,app.newCardRequest?kNewCard:kShow,0,0);
        CloseHandle(app.singleton); app.singleton=nullptr; return false;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    app.icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE));
    if (!app.icon) app.icon=CopyIcon(LoadIconW(nullptr,IDI_APPLICATION));
    WNDCLASSEXW card{}; card.cbSize=sizeof(card); card.style=CS_DBLCLKS;
    card.lpfnWndProc=CardProc; card.hInstance=instance; card.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    card.hIcon=app.icon; card.lpszClassName=kCardClass;
    if (!RegisterClassExW(&card)) return false;
    WNDCLASSEXW controller{}; controller.cbSize=sizeof(controller); controller.lpfnWndProc=ControllerProc;
    controller.hInstance=instance; controller.lpszClassName=kControllerClass;
    if (!RegisterClassExW(&controller)) return false;
    app.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    app.controller=CreateWindowExW(WS_EX_TOOLWINDOW,kControllerClass,app.preview?L"MintDesk Preview":L"MintDesk",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    if (!app.controller) return false;
    if (!app.preview) {
        const bool initialized=desk::InitializeDesktopItems();
        const bool restored=RestoreDesktopWithPermission();
        app.desktopReady=initialized&&restored;
    }
    app.worker=StartWorker();
    desk::AppState stored;
    if (!app.preview && desk::LoadState(stored)) {
        app.nextId=stored.nextId;
        app.opacity=stored.opacity;
        for (auto& state:stored.cards) CreateCard(std::move(state));
    }
    if (app.cards.empty()) DefaultCards();
    if (app.cards.empty()) { PostMessageW(app.controller,WM_CLOSE,0,0); return false; }
    SetOpacity();
    for (auto& c:app.cards) { ShowWindow(c->hwnd,SW_SHOWNOACTIVATE); PlaceAtDesktop(c->hwnd); }
    if (!app.preview&&!app.desktopReady)
        for (auto& c:app.cards) Notify(*c,L"上次的桌面项目未完全恢复，已暂停隐藏；请退出重试");
    if (!app.preview) {
        app.trayAdded=Tray(NIM_ADD);
        if(!desk::EnsureDefaultIntegrations())
            for(auto& c:app.cards)Notify(*c,L"自启动或桌面右键未完成配置，请在托盘中检查");
    }
    if (!app.preview) desk::RefreshDesktopMenu(true);
    app.foregroundHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,ForegroundChanged,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    Dirty(); return true;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int argumentCount=0;
    LPWSTR* arguments=CommandLineToArgvW(GetCommandLineW(),&argumentCount);
    if(arguments&&argumentCount==3&&!_wcsicmp(arguments[1],L"/visibility-watch")) {
        wchar_t* end=nullptr;const unsigned long pid=wcstoul(arguments[2],&end,10);
        const DWORD parent=end&&!*end&&pid?static_cast<DWORD>(pid):0;LocalFree(arguments);
        if(!parent)return 1;
        const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        const int result=desk::RunVisibilityWatcher(parent);
        if(SUCCEEDED(com))CoUninitialize();
        return result;
    }
    if(arguments&&argumentCount>=2&&!_wcsicmp(arguments[1],L"/file-helper")) {
        int result=1;
        if(argumentCount==4) {
            wchar_t* end=nullptr;const unsigned long pid=wcstoul(arguments[2],&end,10);
            if(end&&!*end&&pid)result=desk::RunFileHelper(static_cast<DWORD>(pid),arguments[3]);
        }
        LocalFree(arguments);return result;
    }
    bool exitRequest=false,toggleRequest=false,restoreRequest=false;
    int menuRegistration=0;
    if (arguments) {
        for (int i=1;i<argumentCount;++i) {
            if (!_wcsicmp(arguments[i],L"/preview")) app.preview=true;
            if (!_wcsicmp(arguments[i],L"/exit")) exitRequest=true;
            if (!_wcsicmp(arguments[i],L"/toggle")) toggleRequest=true;
            if (!_wcsicmp(arguments[i],L"/restore-desktop")) restoreRequest=true;
            if (!_wcsicmp(arguments[i],L"/new-card")) app.newCardRequest=true;
            if (!_wcsicmp(arguments[i],L"/register-menu")) menuRegistration=1;
            if (!_wcsicmp(arguments[i],L"/unregister-menu")) menuRegistration=-1;
        }
        LocalFree(arguments);
    }
    if (menuRegistration) return !app.preview && desk::SetDesktopMenu(menuRegistration>0)?0:1;
    if (exitRequest||toggleRequest) {
        HWND window=FindWindowW(kControllerClass,app.preview?L"MintDesk Preview":L"MintDesk");
        if (window) { PostMessageW(window,WM_CLOSE,0,0); return 0; }
        if (exitRequest) return 0;
    }
    const HRESULT com=OleInitialize(nullptr);
    if (FAILED(com)) return 1;
    if (restoreRequest) {
        if (app.preview) { OleUninitialize(); return 1; }
        HANDLE restoreSingleton=CreateMutexW(nullptr,FALSE,L"Local\\DeskCards.Instance.1");
        const DWORD mutexError=GetLastError();
        if (!restoreSingleton||mutexError==ERROR_ALREADY_EXISTS) {
            if (restoreSingleton) CloseHandle(restoreSingleton);
            OleUninitialize(); return 1;
        }
        const bool initialized=desk::InitializeDesktopItems();
        const bool restored=RestoreDesktopWithPermission(); desk::ClearDesktopItems(); desk::StopFileHelper(); OleUninitialize();
        CloseHandle(restoreSingleton);
        return initialized&&restored?0:2;
    }
    int code=0;
    try {
        if (Initialize(instance)) {
            if (app.newCardRequest) HandleCommand(nullptr,NewCard);
            MSG message{};
            while (GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
            code=static_cast<int>(message.wParam);
        }
    } catch (...) {
        code=1;
        if (app.controller) SendMessageW(app.controller,WM_CLOSE,0,0);
    }
    if (app.icon) DestroyIcon(app.icon);
    if (app.singleton) CloseHandle(app.singleton);
    desk::StopFileHelper();
    if (SUCCEEDED(com)) OleUninitialize();
    return code;
}
