// Isolated product-renderer previews. No screen capture or personal data.
#define wWinMain MintDeskApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
#include <gdiplus.h>
#include <cstdio>

namespace {
HFONT DemoFont(int size,int weight=FW_NORMAL) {
    return CreateFontW(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
}
void Caption(HDC dc,const wchar_t* text,RECT area,int size,COLORREF color,int weight=FW_NORMAL) {
    const int saved=SaveDC(dc);SetBkMode(dc,TRANSPARENT);
    HFONT font=DemoFont(size,weight);Text(dc,text,area,font,color,DT_SINGLELINE|DT_VCENTER);DeleteObject(font);RestoreDC(dc,saved);
}
HICON DemoIcon(int kind,COLORREF color) {
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=48;
    info.bmiHeader.biHeight=-48;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return nullptr;}
    auto old=SelectObject(dc,bitmap);std::memset(pixels,0,48*48*4);
    HBRUSH brush=CreateSolidBrush(color);auto previousBrush=SelectObject(dc,brush);auto pen=SelectObject(dc,GetStockObject(NULL_PEN));
    if(kind==0){RoundRect(dc,4,12,27,24,5,5);RoundRect(dc,4,18,44,39,5,5);}
    else {RoundRect(dc,10,5,38,43,5,5);for(int y=17;y<=31;y+=7)Fill(dc,{16,y,32,y+2},RGB(28,66,64));}
    SelectObject(dc,pen);SelectObject(dc,previousBrush);DeleteObject(brush);SelectObject(dc,old);
    auto* colors=static_cast<DWORD*>(pixels);for(int i=0;i<48*48;++i)if(colors[i]&0xffffff)colors[i]|=0xff000000;
    BYTE mask[48*6]{};HBITMAP monochrome=CreateBitmap(48,48,1,1,mask);ICONINFO icon{};icon.fIcon=TRUE;icon.hbmColor=bitmap;icon.hbmMask=monochrome;
    HICON result=CreateIconIndirect(&icon);DeleteObject(monochrome);DeleteObject(bitmap);DeleteDC(dc);return result;
}
Card* DemoCard(const wchar_t* title,desk::Layout layout,int width,int height,const std::vector<std::wstring>& labels) {
    desk::CardState state;state.id=app.nextId++;state.title=title;state.layout=layout;
    Card* card=CreateCard(std::move(state));if(!card)return nullptr;
    ShowWindow(card->hwnd,SW_HIDE);card->placing=true;
    SetWindowPos(card->hwnd,nullptr,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);card->placing=false;card->dpi=96;Fonts(*card);
    const COLORREF palette[]={RGB(102,221,181),RGB(124,192,234),RGB(242,205,124),RGB(184,166,229)};
    for(size_t i=0;i<labels.size();++i){card->state.paths.push_back(L"C:\\Demo\\"+labels[i]);card->items.push_back({DemoIcon(i%3==0?0:1,palette[i%4]),labels[i],app.nextToken++});}
    return card;
}
void Background(HDC dc,int width,int height,const wchar_t* heading,const wchar_t* subtitle) {
    for(int y=0;y<height;++y)Fill(dc,{0,y,width,y+1},Blend(RGB(12,35,34),RGB(18,24,34),y*100/std::max(1,height-1)));
    HPEN p=CreatePen(PS_SOLID,1,RGB(31,65,62));auto old=SelectObject(dc,p);auto brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    Ellipse(dc,width-420,-250,width+380,550);Ellipse(dc,-390,height-310,350,height+430);SelectObject(dc,brush);SelectObject(dc,old);DeleteObject(p);
    Caption(dc,heading,{80,48,width-70,112},42,RGB(229,245,237),FW_SEMIBOLD);
    Caption(dc,subtitle,{82,118,width-70,155},18,RGB(153,188,177));
    Caption(dc,L"MintDesk  /  Windows x64  /  MIT",{82,height-55,width-70,height-20},13,RGB(112,149,142));
}
bool DrawDemo(HDC dc,Card& card,int x,int y,bool reorder=false) {
    if(reorder){app.dragSourceId=card.state.id;card.dropHovered=true;card.selected=1;card.draggingOut=true;const RECT slot=ItemRect(card,4);card.dropPoint={slot.left+1,slot.top+1};}
    desk::AlphaSurface surface;const bool ok=RenderCardSurface(card,surface)&&surface.BlitOver(dc,x,y);app.dragSourceId=0;return ok;
}
bool Save(HBITMAP image,const std::wstring& path) {
    const CLSID encoder={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};
    Gdiplus::Bitmap bitmap(image,nullptr);return bitmap.Save(path.c_str(),&encoder,nullptr)==Gdiplus::Ok;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    const HRESULT com=OleInitialize(nullptr);if(FAILED(com))return 1;
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token=0;if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok){OleUninitialize();return 1;}
    app.preview=true;bool ok=Initialize(GetModuleHandleW(nullptr));
    if(ok){for(auto& c:app.cards)ShowWindow(c->hwnd,SW_HIDE);StopWorker(app.worker);}
    Card* grid=ok?DemoCard(L"工作资料",desk::Layout::Grid,370,352,{L"项目笔记.md",L"待办清单.txt",L"演示图片.png",L"参考资料",L"设计草稿.pdf",L"阅读摘录.md"}):nullptr;
    Card* list=ok?DemoCard(L"日常文件",desk::Layout::List,370,292,{L"今日计划.md",L"会议记录.txt",L"灵感草稿.md",L"资料收集",L"演示图片.png",L"阅读清单.txt",L"临时文档.txt",L"归档资料"}):nullptr;
    Card* archive=ok?DemoCard(L"归档",desk::Layout::List,370,32,{}):nullptr;
    if(archive){archive->state.collapsed=true;archive->state.locked=true;SetWindowPos(archive->hwnd,nullptr,0,0,370,32,SWP_NOZORDER|SWP_NOACTIVATE);}
    constexpr int width=1160,height=700;HDC screen=GetDC(nullptr),canvas=CreateCompatibleDC(screen);HBITMAP image=CreateCompatibleBitmap(screen,width,height);ReleaseDC(nullptr,screen);
    if(canvas&&image&&grid&&list&&archive){auto old=SelectObject(canvas,image);
        Background(canvas,width,height,L"MintDesk  薄荷桌面助手",L"极简 · 开源 · 清爽的薄荷绿");
        ok=DrawDemo(canvas,*grid,150,218)&&DrawDemo(canvas,*list,600,218)&&DrawDemo(canvas,*archive,600,538)&&Save(image,std::wstring(argv[1])+L"\\overview.png");
        Background(canvas,width,height,L"拖动即可排序",L"列表与网格 · 插入位置提示 · 自动保存顺序");
        ok=DrawDemo(canvas,*list,150,218,true)&&DrawDemo(canvas,*grid,600,218,true)&&Save(image,std::wstring(argv[1])+L"\\reorder.png")&&ok;
        SelectObject(canvas,old);
    }else ok=false;
    if(image)DeleteObject(image);
    if(canvas)DeleteDC(canvas);
    if(app.controller)SendMessageW(app.controller,WM_CLOSE,0,0);
    if(app.icon)DestroyIcon(app.icon);
    if(app.singleton)CloseHandle(app.singleton);
    Gdiplus::GdiplusShutdown(token);OleUninitialize();
    if(ok)std::puts("Rendered fictional MintDesk previews; no desktop capture or personal configuration was used.");
    return ok?0:1;
}
