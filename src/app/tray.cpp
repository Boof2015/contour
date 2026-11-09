#include "tray.h"
#include "resource.h"
#include <wrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>

namespace contour::app {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
constexpr UINT tray_event=WM_APP+11,tray_menu=WM_APP+12;
constexpr auto tray_page=L"https://contour.invalid/tray.html";
HICON make_icon() {
    // Use the same embedded C as Explorer and the app window. Its dark backing
    // keeps the mark readable on both light and dark taskbars.
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_CONTOUR),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0));
}
}
TrayHost::TrayHost(HWND owner,std::filesystem::path assets,std::function<void(const Json&)> action)
    :owner_(owner),assets_(std::move(assets)),action_(std::move(action)),taskbar_created_(RegisterWindowMessageW(L"TaskbarCreated")) {}
TrayHost::~TrayHost() {
    NOTIFYICONDATAW n{sizeof(n)};n.hWnd=owner_;n.uID=1;Shell_NotifyIconW(NIM_DELETE,&n);
    if(controller_)controller_->Close();
    if(window_)DestroyWindow(window_);
    if(icon_)DestroyIcon(icon_);
}
void TrayHost::initialize(ICoreWebView2Environment* environment) { environment_=environment;add_icon(); }
void TrayHost::add_icon() {
    if(!icon_)icon_=make_icon();
    if(!icon_)return;
    NOTIFYICONDATAW n{sizeof(n)};n.hWnd=owner_;n.uID=1;n.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
    n.uCallbackMessage=tray_event;n.hIcon=icon_;wcscpy_s(n.szTip,L"Contour");
    installed_=Shell_NotifyIconW(NIM_ADD,&n)!=FALSE;
    if(installed_){n.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&n);}
}
bool TrayHost::handle(UINT message,WPARAM,LPARAM l) {
    if(taskbar_created_&&message==taskbar_created_){installed_=false;add_icon();if(!installed_)action_({{"action","open-editor"}});return true;}
    if(message==tray_menu){menu();return true;}
    if(message!=tray_event)return false;
    const auto event=LOWORD(l);
    if(event==NIN_SELECT||event==NIN_KEYSELECT){
        if((window_&&IsWindowVisible(window_))||GetTickCount64()-dismissed_<180)hide();
        else action_({{"action","open"}});
    }else if(event==WM_CONTEXTMENU)PostMessageW(owner_,tray_menu,0,0);
    return true;
}
void TrayHost::menu() {
    hide();const auto menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING,1,L"Open Contour");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,2,L"Quit");
    POINT p{};GetCursorPos(&p);SetForegroundWindow(owner_);
    const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,owner_,nullptr);
    DestroyMenu(menu);PostMessageW(owner_,WM_NULL,0,0);
    if(selected)action_({{"action",selected==1?"open-editor":"quit"}});
    else {NOTIFYICONDATAW n{sizeof(n)};n.hWnd=owner_;n.uID=1;Shell_NotifyIconW(NIM_SETFOCUS,&n);}
}
void TrayHost::show() {
    if(!environment_)return;
    requested_=true;
    if(!window_)create();
    if(ready_)send_state();
}
void TrayHost::hide(bool return_focus) {
    const bool was_requested=requested_;
    requested_=false;
    if(window_&&IsWindowVisible(window_)){ShowWindow(window_,SW_HIDE);dismissed_=GetTickCount64();}
    if(controller_)controller_->put_IsVisible(FALSE);
    if(was_requested)action_({{"action","dismissed"}});
    if(return_focus){NOTIFYICONDATAW n{sizeof(n)};n.hWnd=owner_;n.uID=1;Shell_NotifyIconW(NIM_SETFOCUS,&n);}
}
void TrayHost::update(const Json& state) {
    state_=state;light_=state.value("theme",std::string{})=="light";
    if(controller_){ComPtr<ICoreWebView2Controller2> bg;if(SUCCEEDED(controller_.As(&bg)))bg->put_DefaultBackgroundColor(light_?COREWEBVIEW2_COLOR{255,255,255,255}:COREWEBVIEW2_COLOR{255,38,38,42});}
    if(ready_)send_state();
}
void TrayHost::send_state() {
    if(view_&&!state_.is_null()){const auto text=wide(state_.dump());view_->PostWebMessageAsJson(text.c_str());}
}
void TrayHost::layout(int height) {
    if(!window_||!controller_)return;
    NOTIFYICONIDENTIFIER id{sizeof(id)};id.hWnd=owner_;id.uID=1;RECT anchor{};
    if(FAILED(Shell_NotifyIconGetRect(&id,&anchor))){POINT p{};GetCursorPos(&p);anchor={p.x,p.y,p.x+1,p.y+1};}
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor);
    // Move to the icon's monitor first so per-monitor DPI determines the size.
    if(!IsWindowVisible(window_))SetWindowPos(window_,nullptr,anchor.left,anchor.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    const auto dpi=static_cast<int>(GetDpiForWindow(window_)),gap=MulDiv(8,dpi,96);
    const int width=std::min(MulDiv(368,dpi,96),static_cast<int>(monitor.rcWork.right-monitor.rcWork.left)-2*gap);
    const int h=std::min(MulDiv(std::clamp(height,160,640),dpi,96),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top)-2*gap);
    const auto x=std::clamp(static_cast<int>(anchor.right)-width,static_cast<int>(monitor.rcWork.left)+gap,static_cast<int>(monitor.rcWork.right)-width-gap);
    int y=static_cast<int>(anchor.top)-h-gap;
    if(y<monitor.rcWork.top)y=static_cast<int>(anchor.bottom)+gap;
    y=std::clamp(y,static_cast<int>(monitor.rcWork.top)+gap,static_cast<int>(monitor.rcWork.bottom)-h-gap);
    SetWindowPos(window_,HWND_TOPMOST,x,y,width,h,SWP_NOACTIVATE);
    if(requested_&&!IsWindowVisible(window_)){
        controller_->put_IsVisible(TRUE);ShowWindow(window_,SW_SHOWNORMAL);SetForegroundWindow(window_);
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}
void TrayHost::receive(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    try{
        wchar_t* source=nullptr;args->get_Source(&source);const bool trusted=source&&std::wstring_view(source)==tray_page;CoTaskMemFree(source);if(!trusted)return;
        wchar_t* raw=nullptr;if(FAILED(args->TryGetWebMessageAsString(&raw)))return;
        const std::wstring body=raw?raw:L"";CoTaskMemFree(raw);if(body.size()>8192)return;
        const auto request=Json::parse(utf8(body));const auto action=request.at("action").get<std::string>();
        if(action=="ready"){ready_=true;send_state();return;}
        if(action=="layout"){layout(request.at("height").get<int>());return;}
        if(action=="dismiss"){hide(true);return;}
        if(action=="open-editor"||action=="eq"||action=="device"||action=="profile"||action=="layer"||action=="save"||action=="revert")action_(request);
    }catch(const std::exception&){} // An invalid view message cannot mutate the model.
}
void TrayHost::create() {
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW cls{};cls.hInstance=instance;cls.lpszClassName=L"ContourTray";cls.lpfnWndProc=proc;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
    window_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,L"Contour quick controls",WS_POPUP|WS_CLIPCHILDREN,0,0,368,640,nullptr,nullptr,instance,this);
    if(!window_){requested_=false;action_({{"action","open-editor"}});return;}
    const DWORD round=2;DwmSetWindowAttribute(window_,33,&round,sizeof(round));
    const auto start=environment_->CreateCoreWebView2Controller(window_,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this](HRESULT hr,ICoreWebView2Controller* c)->HRESULT {
        if(FAILED(hr)||!c){DestroyWindow(window_);window_=nullptr;requested_=false;action_({{"action","open-editor"}});return S_OK;}
        controller_=c;controller_->get_CoreWebView2(&view_);controller_->put_IsVisible(FALSE);
        RECT bounds{};GetClientRect(window_,&bounds);controller_->put_Bounds(bounds);update(state_);
        ComPtr<ICoreWebView2Settings> settings;view_->get_Settings(&settings);
        settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_AreDevToolsEnabled(FALSE);settings->put_IsStatusBarEnabled(FALSE);settings->put_AreHostObjectsAllowed(FALSE);settings->put_AreDefaultScriptDialogsEnabled(FALSE);settings->put_IsZoomControlEnabled(FALSE);
        ComPtr<ICoreWebView2_3> local;
        if(FAILED(view_.As(&local))||FAILED(local->SetVirtualHostNameToFolderMapping(L"contour.invalid",assets_.c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))){hide();action_({{"action","open-editor"}});return E_FAIL;}
        EventRegistrationToken token{};
        view_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* e)->HRESULT {
            wchar_t* uri=nullptr;e->get_Uri(&uri);if(!uri||std::wstring_view(uri)!=tray_page)e->put_Cancel(TRUE);CoTaskMemFree(uri);return S_OK;
        }).Get(),&token);
        view_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* e)->HRESULT {e->put_Handled(TRUE);return S_OK;}).Get(),&token);
        view_->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* e)->HRESULT {e->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);return S_OK;}).Get(),&token);
        view_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* e)->HRESULT {receive(e);return S_OK;}).Get(),&token);
        view_->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([this](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT {ready_=false;hide();action_({{"action","open-editor"}});view_->Reload();return S_OK;}).Get(),&token);
        const auto navigation=view_->Navigate(tray_page);
        if(FAILED(navigation)){hide();action_({{"action","open-editor"}});}
        return navigation;
    }).Get());
    if(FAILED(start)){DestroyWindow(window_);window_=nullptr;requested_=false;action_({{"action","open-editor"}});}
}
LRESULT CALLBACK TrayHost::proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* self=reinterpret_cast<TrayHost*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<TrayHost*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(window,message,w,l);
    switch(message){
    case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE)self->hide();return 0;
    case WM_CLOSE:self->hide(true);return 0;
    case WM_SIZE:if(self->controller_){RECT r{};GetClientRect(window,&r);self->controller_->put_Bounds(r);}return 0;
    case WM_MOVE:if(self->controller_)self->controller_->NotifyParentWindowPositionChanged();return 0;
    case WM_ERASEBKGND:{RECT r{};GetClientRect(window,&r);const auto dc=reinterpret_cast<HDC>(w);const auto old=SetDCBrushColor(dc,self->light_?RGB(255,255,255):RGB(38,38,42));FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));SetDCBrushColor(dc,old);return 1;}
    }
    return DefWindowProcW(window,message,w,l);
}
}
