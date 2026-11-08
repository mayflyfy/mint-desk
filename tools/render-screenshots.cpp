// Production card rendering with real Windows Shell icons and isolated samples.
// No personal desktop, installed-app layout or generated artwork is used.
#define wWinMain MintDeskApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
#include <gdiplus.h>
#include <cstdio>

namespace {
struct Samples {
    std::wstring root;
    std::vector<std::wstring> files, directories;
    Samples() {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporary)) return;
        root = std::wstring(temporary) + L"MintDesk-screenshot-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        if (!CreateDirectoryW(root.c_str(), nullptr)) root.clear();
    }
    ~Samples() {
        for (const auto& path : files) DeleteFileW(path.c_str());
        for (const auto& path : directories) RemoveDirectoryW(path.c_str());
        if (!root.empty()) RemoveDirectoryW(root.c_str());
    }
    std::wstring File(const wchar_t* name) {
        if (root.empty()) return {};
        const auto path = root + L"\\" + name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        CloseHandle(file); files.push_back(path); return path;
    }
    std::wstring Folder(const wchar_t* name) {
        if (root.empty()) return {};
        const auto path = root + L"\\" + name;
        if (!CreateDirectoryW(path.c_str(), nullptr)) return {};
        directories.push_back(path); return path;
    }
    std::wstring Link(const wchar_t* name, const std::wstring& target) {
        if (root.empty()) return {};
        const auto path = root + L"\\" + name + L".lnk";
        IShellLinkW* link = nullptr; IPersistFile* file = nullptr;
        bool okay = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(&link)));
        if (okay) okay = SUCCEEDED(link->SetPath(target.c_str())) &&
            SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) &&
            SUCCEEDED(file->Save(path.c_str(), TRUE));
        if (file) file->Release();
        if (link) link->Release();
        if (!okay) return {};
        files.push_back(path); return path;
    }
};
void Caption(HDC dc, const wchar_t* text, RECT area, int size, COLORREF color, int weight = FW_NORMAL) {
    const int saved = SaveDC(dc); SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    Text(dc, text, area, font, color, DT_SINGLELINE | DT_VCENTER);
    DeleteObject(font); RestoreDC(dc, saved);
}
Card* DemoCard(const wchar_t* title, desk::Layout layout, int height,
              const std::vector<std::wstring>& paths, bool locked = false, bool collapsed = false) {
    desk::CardState state; state.id = app.nextId++; state.title = title; state.layout = layout;
    state.locked = locked; state.collapsed = collapsed;
    Card* card = CreateCard(std::move(state)); if (!card) return nullptr;
    ShowWindow(card->hwnd, SW_HIDE); card->placing = true;
    SetWindowPos(card->hwnd, nullptr, 0, 0, 440, height, SWP_NOZORDER | SWP_NOACTIVATE);
    card->placing = false; card->dpi = 120; Fonts(*card);
    for (const auto& path : paths) {
        if (path.empty()) return nullptr;
        auto visual = desk::LoadShellVisual(path, Dip(*card, layout == desk::Layout::Grid ? 40 : 16));
        if (!visual.icon) return nullptr;
        card->state.paths.push_back(path);
        card->items.push_back({visual.icon, FallbackLabel(path), app.nextToken++});
    }
    return card;
}
void Background(HDC dc, const wchar_t* heading, const wchar_t* subtitle) {
    for (int y = 0; y < 1000; ++y)
        Fill(dc, {0, y, 1440, y + 1}, Blend(RGB(23, 48, 48), RGB(20, 27, 39), y / 10));
    Caption(dc, heading, {44, 24, 1396, 85}, 37, RGB(234, 245, 240), FW_SEMIBOLD);
    Caption(dc, subtitle, {46, 89, 1396, 119}, 18, RGB(168, 189, 185));
}
bool DrawCard(HDC dc, Card& card, int x, int y) {
    desk::AlphaSurface surface;
    return RenderCardSurface(card, surface) && surface.BlitOver(dc, x, y);
}
void DragIcon(HDC dc, Card& source, size_t index, int x, int y) {
    desk::AlphaSurface image; const int size = Dip(source, 40);
    if (!image.Create(size, size)) return;
    for (HDC matte : {image.BlackDC(), image.WhiteDC()})
        DrawIconEx(matte, 0, 0, source.items[index].icon, size, size, 0, nullptr, DI_NORMAL);
    if (image.Compose(RGB(0, 0, 0), 0)) image.BlitOver(dc, x, y);
    DrawIconEx(dc, x + size / 2, y + size / 2, LoadCursorW(nullptr, IDC_ARROW), 24, 24, 0, nullptr, DI_NORMAL);
}
bool SaveImage(HBITMAP image, const std::wstring& path) {
    const CLSID encoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
    Gdiplus::Bitmap bitmap(image, nullptr);
    return bitmap.Save(path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const HRESULT com = OleInitialize(nullptr); if (FAILED(com)) return 1;
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { OleUninitialize(); return 1; }
    Samples samples; app.preview = true;
    bool okay = !samples.root.empty() && Initialize(GetModuleHandleW(nullptr));
    if (okay) { for (auto& card : app.cards) ShowWindow(card->hwnd, SW_HIDE); StopWorker(app.worker); }
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    PWSTR programFiles = nullptr; SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &programFiles);
    const auto edge = std::wstring(programFiles ? programFiles : L"C:\\Program Files (x86)") +
        L"\\Microsoft\\Edge\\Application\\msedge.exe";
    if (programFiles) CoTaskMemFree(programFiles);
    const auto document = samples.File(L"项目方案.docx"), sheet = samples.File(L"预算清单.xlsx");
    const auto slides = samples.File(L"产品汇报.pptx"), pdf = samples.File(L"交付说明.pdf");
    const auto notes = samples.File(L"阅读笔记.md"), todo = samples.File(L"待办清单.txt");
    const auto project = samples.Folder(L"项目资料"), assets = samples.Folder(L"素材库");
    const auto previous = samples.Folder(L"历史版本"), source = samples.Folder(L"源代码");
    const auto archiveFile = samples.File(L"项目归档.zip");
    Card* documents = okay ? DemoCard(L"工作文档", desk::Layout::Grid, 315,
        {document, sheet, slides, pdf, notes, todo}) : nullptr;
    Card* applications = okay ? DemoCard(L"常用应用", desk::Layout::Grid, 315,
        {samples.Link(L"文件资源管理器", std::wstring(windows) + L"\\explorer.exe"),
         samples.Link(L"记事本", std::wstring(windows) + L"\\System32\\notepad.exe"), samples.Link(L"Microsoft Edge", edge),
         samples.Link(L"计算器", std::wstring(windows) + L"\\System32\\calc.exe"),
         samples.Link(L"画图", std::wstring(windows) + L"\\System32\\mspaint.exe"),
         samples.Link(L"控制面板", std::wstring(windows) + L"\\System32\\control.exe")}) : nullptr;
    Card* projects = okay ? DemoCard(L"项目与目录", desk::Layout::List, 315,
        {project, assets, previous, source, document, sheet, slides, archiveFile}) : nullptr;
    Card* media = okay ? DemoCard(L"设计素材", desk::Layout::Grid, 315,
        {samples.File(L"首页草图.png"), samples.File(L"产品照片.jpg"), samples.File(L"横幅设计.svg"),
         samples.File(L"演示视频.mp4"), samples.File(L"示例动画.gif"), assets}) : nullptr;
    Card* reading = okay ? DemoCard(L"待办与阅读", desk::Layout::List, 315,
        {todo, samples.File(L"会议纪要.docx"), samples.File(L"迭代计划.xlsx"), pdf, notes,
         samples.File(L"需求清单.docx"), samples.File(L"参考资料.pdf"), samples.File(L"发布检查.txt"),
         samples.File(L"开发说明.md"), samples.File(L"版本记录.txt")}) : nullptr;
    Card* pinned = okay ? DemoCard(L"常用资料 · 已锁定", desk::Layout::Grid, 315,
        {project, samples.Folder(L"学习资料"), archiveFile, pdf, samples.File(L"常用表格.xlsx"), source}, true) : nullptr;
    Card* collapsed = okay ? DemoCard(L"历史归档 · 已收起", desk::Layout::List, 40,
        {previous, archiveFile, samples.File(L"上一版汇报.pptx")}, true, true) : nullptr;
    HDC screen = GetDC(nullptr), canvas = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, 1440, 1000); ReleaseDC(nullptr, screen);
    if (canvas && image && documents && applications && projects && media && reading && pinned && collapsed) {
        auto old = SelectObject(canvas, image);
        auto scene = [&]() { return DrawCard(canvas, *documents, 44, 145) && DrawCard(canvas, *applications, 500, 145) &&
            DrawCard(canvas, *projects, 956, 145) && DrawCard(canvas, *media, 44, 476) && DrawCard(canvas, *reading, 500, 476) &&
            DrawCard(canvas, *pinned, 956, 476) && DrawCard(canvas, *collapsed, 44, 807); };
        Background(canvas, L"薄荷桌面助手  /  MintDesk", L"真实 Windows 图标 · 文档、应用与文件夹，一眼找到 · 原生界面示例");
        okay = scene();
        Caption(canvas, L"网格 / 列表切换   ·   锁定位置   ·   收起展开   ·   自由缩放", {506, 806, 1396, 849}, 18, kText);
        Caption(canvas, L"拖入整理  ·  拖动排序  ·  跨卡片移动 / Ctrl 复制  ·  双击打开 / 右键定位", {46, 879, 1396, 909}, 18, kText);
        Caption(canvas, L"边缘吸附与对齐线  ·  多屏适配  ·  自动保存布局  ·  桌面原项隐藏 / 退出恢复", {46, 917, 1396, 947}, 17, kMuted);
        Caption(canvas, L"开机启动  /  桌面右键  /  背景透明度可选     ·     v0.9.1  Windows x64  ·  MIT", {46, 954, 1396, 980}, 16, kMuted);
        okay = SaveImage(image, std::wstring(argv[1]) + L"\\overview.png") && okay;
        Background(canvas, L"拖动即可整理 · 入口与原文件分开管理", L"拖动时显示真实图标，插入线提示排序位置；跨卡片移动，按住 Ctrl 保留源入口。");
        documents->selected = 2; documents->draggingOut = true; documents->dropHovered = true;
        app.dragSourceId = documents->state.id;
        const RECT slot = ItemRect(*documents, 4); documents->dropPoint = {slot.left + 2, slot.top + 2}; projects->dropHovered = true;
        okay = scene() && okay;
        DragIcon(canvas, *documents, 2, 44 + slot.left + 36, 145 + slot.top + 28); app.dragSourceId = 0;
        Caption(canvas, L"卡片内：插入线标记新位置        跨卡片：目标卡片高亮        Ctrl：复制入口", {46, 873, 1396, 912}, 19, kText);
        Caption(canvas, L"删除入口不删除文件  ·  软件卸载后自动清理失效引用  ·  网络与移动磁盘不可用时保留入口", {46, 920, 1396, 951}, 17, kMuted);
        Caption(canvas, L"画面使用实际程序渲染与本机 Shell 图标；文件名和目录均为独立示例。", {46, 961, 1396, 985}, 15, kMuted);
        okay = SaveImage(image, std::wstring(argv[1]) + L"\\reorder.png") && okay;
        SelectObject(canvas, old);
    } else okay = false;
    if (image) DeleteObject(image);
    if (canvas) DeleteDC(canvas);
    if (app.controller) SendMessageW(app.controller, WM_CLOSE, 0, 0);
    if (app.icon) DestroyIcon(app.icon);
    if (app.singleton) CloseHandle(app.singleton);
    Gdiplus::GdiplusShutdown(token); OleUninitialize();
    if (okay) std::puts("Rendered compact native cards using real Windows icons and isolated sample files.");
    return okay ? 0 : 1;
}
