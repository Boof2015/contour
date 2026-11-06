#include "model.h"
#include "setup.h"
#include <WebView2.h>
#include <wrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using contour::app::Json;
constexpr auto page=L"https://contour.invalid/index.html";
constexpr UINT start_frame_drag=WM_APP+1,open_system_menu=WM_APP+2;
struct App {
    HWND window=nullptr;
    std::unique_ptr<contour::app::Model> model;
    std::unique_ptr<contour::app::DeviceSetup> setup;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::filesystem::path assets,data;
    bool ready=false,custom_frame=false;
    RECT view_bounds{-1,-1,-1,-1};
    ULONGLONG caption_click=0; POINT caption_point{};
    Json sent_window_state;
    COLORREF background_color=RGB(22,22,24);
    void apply_background() {
        ComPtr<ICoreWebView2Controller2> background;
        if (controller && SUCCEEDED(controller.As(&background))) {
            background->put_DefaultBackgroundColor({255,GetRValue(background_color),GetGValue(background_color),GetBValue(background_color)});
        }
    }
    Json window_state() const { return {{"custom",custom_frame},{"maximized",IsZoomed(window)!=FALSE}}; }
    void send_window_state(bool force=false) {
        const auto state=window_state();
        if (force || state!=sent_window_state) { sent_window_state=state; send({{"event","window"},{"data",state}}); }
    }
    void send(const Json& j) {
        if (!view) { return; }
        wchar_t* source=nullptr; const auto hr=view->get_Source(&source);
        const bool trusted=SUCCEEDED(hr) && source && std::wstring_view(source)==page;
        CoTaskMemFree(source);
        if (trusted) { const auto value=contour::app::wide(j.dump()); view->PostWebMessageAsJson(value.c_str()); }
    }
    void resize() {
        if (!controller || IsIconic(window)) { return; }
        RECT rect{}; GetClientRect(window,&rect);
        if (!EqualRect(&rect,&view_bounds) && SUCCEEDED(controller->put_Bounds(rect))) { view_bounds=rect; }
    }
    void fail(const std::string& error) {
        MessageBoxW(window,contour::app::wide(error).c_str(),L"Contour",MB_OK|MB_ICONERROR);
    }
    void message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
        Json request_id=nullptr;
        try {
            wchar_t* source=nullptr; if (FAILED(args->get_Source(&source))) { return; }
            const bool trusted=source && std::wstring_view(source)==page; CoTaskMemFree(source); if (!trusted) { return; }
            wchar_t* raw=nullptr; if (FAILED(args->TryGetWebMessageAsString(&raw))) { return; }
            std::wstring body=raw ? raw : L""; CoTaskMemFree(raw);
            if (body.size()>2*1024*1024) { throw std::invalid_argument("Editor message is too large"); }
            const auto request=Json::parse(contour::app::utf8(body)); request_id=request.at("requestId");
            if (!request_id.is_number_unsigned()) { throw std::invalid_argument("Invalid request identity"); }
            if (request.at("op")=="window") {
                const auto action=request.at("action").get<std::string>();
                if (action=="minimize") { ShowWindow(window,SW_MINIMIZE); }
                else if (action=="maximize") { ShowWindow(window,IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE); }
                else if (action=="close") { PostMessageW(window,WM_CLOSE,0,0); }
                else if (action=="theme") {
                    const auto theme=request.at("theme").get<std::string>();
                    if (theme!="dark" && theme!="light") { throw std::invalid_argument("Unknown theme"); }
                    background_color=theme=="light" ? RGB(241,241,243) : RGB(22,22,24);
                    apply_background(); InvalidateRect(window,nullptr,TRUE);
                }
                else if (action=="resize-top" || action=="resize-top-left" || action=="resize-top-right") {
                    // The WebView covers the top edge. Its transparent hit area
                    // starts Windows' normal resize loop without painting a bar.
                    if (custom_frame && !IsZoomed(window)) {
                        const auto hit=action=="resize-top-left" ? HTTOPLEFT : action=="resize-top-right" ? HTTOPRIGHT : HTTOP;
                        POINT cursor{}; GetCursorPos(&cursor);
                        PostMessageW(window,start_frame_drag,hit,MAKELPARAM(cursor.x,cursor.y));
                    }
                }
                else if (action=="move" && custom_frame) {
                    // Synthesized caption presses never produce WM_NCLBUTTONDBLCLK,
                    // so a second press within the system double-click limits toggles
                    // maximize the way a native caption does.
                    POINT cursor{}; GetCursorPos(&cursor); const auto now=GetTickCount64();
                    const bool second=now-caption_click<=GetDoubleClickTime() &&
                        std::abs(cursor.x-caption_point.x)*2<=GetSystemMetrics(SM_CXDOUBLECLK) &&
                        std::abs(cursor.y-caption_point.y)*2<=GetSystemMetrics(SM_CYDOUBLECLK);
                    caption_click=second ? 0 : now; caption_point=cursor;
                    if (second) { ShowWindow(window,IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE); }
                    else { PostMessageW(window,start_frame_drag,HTCAPTION,MAKELPARAM(cursor.x,cursor.y)); }
                }
                else if (action=="menu" && custom_frame) {
                    // TrackPopupMenu runs a modal loop; keep it out of this WebView event.
                    POINT cursor{}; GetCursorPos(&cursor);
                    PostMessageW(window,open_system_menu,0,MAKELPARAM(cursor.x,cursor.y));
                }
                else { throw std::invalid_argument("Unknown window action"); }
                send({{"requestId",request_id},{"ok",true},{"data",window_state()}}); return;
            }
            if (request.at("op")=="setup") {
                if (model->state(false)["closing"]["state"]!="open") { throw std::runtime_error("Finish or cancel closing before device setup"); }
                const auto endpoint=contour::app::wide(request.at("endpoint").get<std::string>());
                const auto* device=model->find_device(endpoint);
                if (!device) { throw std::invalid_argument("This device is no longer in the device list. Refresh the list and try again"); }
                const auto action=request.at("action").get<std::string>();
                if (action=="Install" && !device->available) { throw std::runtime_error("Connect this device before installing Contour"); }
                const auto result=setup->request(window,device->id,action);
                model->suspend_connections(setup->mutating());
                send({{"requestId",request_id},{"ok",true},{"data",result}}); return;
            }
            if (setup->mutating() && request.at("op")!="state" && request.at("op")!="inspect") {
                throw std::runtime_error("Wait for installation or removal to finish");
            }
            auto result=model->dispatch(request); ready=true;
            send({{"requestId",request_id},{"ok",true},{"data",std::move(result)}});
            if (request.at("op")=="state") { send_window_state(true); }
            if (model->closed()) { DestroyWindow(window); }
        } catch (const std::exception& e) {
            try { send({{"requestId",request_id},{"ok",false},{"error",e.what()},{"state",model->state()}}); } catch (...) {}
        }
    }
    void initialize() {
        const auto cache=(data/L"webview").wstring();
        const auto hr=CreateCoreWebView2EnvironmentWithOptions(nullptr,cache.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT result,ICoreWebView2Environment* environment)->HRESULT {
                if (FAILED(result) || !environment) { fail("The Microsoft Edge WebView2 Runtime could not start."); return S_OK; }
                return environment->CreateCoreWebView2Controller(window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this](HRESULT created,ICoreWebView2Controller* control)->HRESULT {
                        if (FAILED(created) || !control) { fail("Contour could not create its editor window."); return S_OK; }
                        controller=control; controller->get_CoreWebView2(&view); resize();
                        apply_background();
                        ComPtr<ICoreWebView2Settings> settings; view->get_Settings(&settings);
                        settings->put_AreDefaultContextMenusEnabled(FALSE); settings->put_AreDevToolsEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE); settings->put_AreHostObjectsAllowed(FALSE);
                        settings->put_AreDefaultScriptDialogsEnabled(FALSE); settings->put_IsZoomControlEnabled(FALSE);
                        // The page starts caption drags through "move". WebView2's
                        // app-region support stays off: it blocked this thread ~20 ms
                        // whenever the title bar's width changed, so resizing advanced
                        // in large steps and each async WebView frame visibly jumped.
                        custom_frame=true;
                        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
                        resize();
                        ComPtr<ICoreWebView2_3> local;
                        if (FAILED(view.As(&local)) || FAILED(local->SetVirtualHostNameToFolderMapping(L"contour.invalid",assets.c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))) {
                            fail("Contour requires a newer Microsoft Edge WebView2 Runtime."); return S_OK;
                        }
                        EventRegistrationToken token{};
                        view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* e)->HRESULT {
                            wchar_t* uri=nullptr; e->get_Uri(&uri);
                            if (!uri || std::wstring_view(uri)!=page) { e->put_Cancel(TRUE); }
                            CoTaskMemFree(uri); return S_OK;
                        }).Get(),&token);
                        view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* e)->HRESULT { e->put_Handled(TRUE); return S_OK; }).Get(),&token);
                        view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* e)->HRESULT { e->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK; }).Get(),&token);
                        view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* e)->HRESULT { message(e); return S_OK; }).Get(),&token);
                        view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([this](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT {
                            ready=false; fail("The editor display stopped. Your draft remains in memory. Close the window to choose whether to save it."); return S_OK;
                        }).Get(),&token);
                        return view->Navigate(page);
                    }).Get());
            }).Get());
        if (FAILED(hr)) { fail("Install the Microsoft Edge WebView2 Runtime to open Contour."); }
    }
};
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); app->window=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) { return DefWindowProcW(window,message,w,l); }
    switch (message) {
    case WM_ERASEBKGND: {
        // WS_CLIPCHILDREN excludes the WebView. Only uncovered host pixels are
        // filled, using the same theme rather than flashing a black/white brush.
        const auto dc=reinterpret_cast<HDC>(w); RECT rect{}; GetClientRect(window,&rect);
        const auto old=SetDCBrushColor(dc,app->background_color);
        FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc,old); return 1;
    }
    case WM_NCCALCSIZE:
        if (w && app->custom_frame) {
            auto* params=reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            const auto top=params->rgrc[0].top;
            // Retain the side/bottom frame, but draw the toolbar all the way to
            // the top. Maximized windows still need their offscreen frame inset.
            DefWindowProcW(window,message,w,l);
            const auto dpi=GetDpiForWindow(window);
            params->rgrc[0].top=top+(IsZoomed(window) ? GetSystemMetricsForDpi(SM_CYFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi) : 0);
            // Keep client bits anchored to the client origin. With WVR_REDRAW a
            // top-edge drag showed the WebView at its old screen position for a
            // frame, so the whole UI jumped by the drag distance and snapped back.
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (app->custom_frame && !IsZoomed(window)) {
            RECT rect{}; GetWindowRect(window,&rect);
            const auto dpi=GetDpiForWindow(window);
            const auto border=GetSystemMetricsForDpi(SM_CYFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
            const auto x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
            if (y>=rect.top && y<rect.top+border) {
                if (x<rect.left+border) { return HTTOPLEFT; }
                if (x>=rect.right-border) { return HTTOPRIGHT; }
                return HTTOP;
            }
        }
        break;
    case start_frame_drag:
        // A quick click can be released before this posted message runs. A move
        // or size loop started then would leave the window following the pointer.
        if (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON)<0) {
            ReleaseCapture(); DefWindowProcW(window,WM_NCLBUTTONDOWN,w,l);
        }
        return 0;
    case open_system_menu: {
        const auto menu=GetSystemMenu(window,FALSE); const bool zoomed=IsZoomed(window)!=FALSE;
        EnableMenuItem(menu,SC_RESTORE,MF_BYCOMMAND|(zoomed ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menu,SC_MOVE,MF_BYCOMMAND|(zoomed ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(menu,SC_SIZE,MF_BYCOMMAND|(zoomed ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(menu,SC_MAXIMIZE,MF_BYCOMMAND|(zoomed ? MF_GRAYED : MF_ENABLED));
        const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,GET_X_LPARAM(l),GET_Y_LPARAM(l),0,window,nullptr);
        if (command) { PostMessageW(window,WM_SYSCOMMAND,static_cast<WPARAM>(command),0); }
        return 0;
    }
    case WM_SIZE: app->resize(); if (app->ready) { app->send_window_state(); } return 0;
    case WM_MOVE: if (app->controller) { app->controller->NotifyParentWindowPositionChanged(); } break;
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(l); const auto dpi=GetDpiForWindow(window);
        info->ptMinTrackSize={MulDiv(960,static_cast<int>(dpi),96),MulDiv(640,static_cast<int>(dpi),96)}; return 0;
    }
    case WM_DPICHANGED: {
        const auto* rect=reinterpret_cast<RECT*>(l);
        SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0;
    }
    case WM_TIMER:
        if (app->setup->poll() && app->ready) {
            const auto& report=app->setup->state();
            app->model->suspend_connections(app->setup->mutating());
            if(!report.value("busy",false) && report.value("state",std::string{})=="attached") {
                app->model->check_connection(contour::app::wide(report.at("endpoint").get<std::string>()));
            }
            app->send({{"event","setup"},{"data",report}});
        }
        if (app->ready) {
            try { app->model->poll(); app->send({{"event","status"},{"data",app->model->state(false)}}); if (app->model->closed()) { DestroyWindow(window); } }
            catch (const std::exception& e) { app->send({{"event","error"},{"error",e.what()}}); }
        }
        return 0;
    case WM_CLOSE:
        if (app->setup->mutating()) { app->fail("Wait for installation or removal to finish before closing Contour."); return 0; }
        if (app->ready) { app->send({{"event","close"}}); }
        else {
            // WebView failure must not bypass the same draft/save guarantees.
            try {
                auto state=app->model->dispatch({{"op","close"}});
                if (state["closing"]["state"]=="needs_decision") {
                    const auto answer=MessageBoxW(window,L"Save your changes before closing?",L"Contour",MB_YESNOCANCEL|MB_ICONQUESTION);
                    state=app->model->dispatch({{"op","close"},{"choice",answer==IDYES ? "save" : answer==IDNO ? "discard" : "cancel"}});
                }
                if (app->model->closed()) { DestroyWindow(window); }
                else if (state["closing"]["state"]!="open") {
                    app->fail("The device update has not completed. The editor remains open; reopen the display to retry or explicitly leave the update pending.");
                    if (app->view) { app->view->Reload(); }
                }
            } catch (const std::exception& e) { app->fail(e.what()); }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window,1); if (app->controller) { app->controller->Close(); }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) { return 1; }
    int exit_code=0;
    try {
        App app;
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
        app.assets=std::filesystem::path(executable).parent_path()/L"ui";
        app.setup=std::make_unique<contour::app::DeviceSetup>(std::filesystem::path(executable).parent_path());
        wchar_t* local=nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))) { throw std::runtime_error("Cannot find local application storage"); }
        app.data=std::filesystem::path(local)/L"Contour"; CoTaskMemFree(local);
        int count=0; auto** args=CommandLineToArgvW(GetCommandLineW(),&count);
        if (count==3 && std::wstring_view(args[1])==L"--data-dir") { app.data=std::filesystem::absolute(args[2]); }
        else if (count!=1) { LocalFree(args); throw std::invalid_argument("Usage: contour.exe [--data-dir directory]"); }
        LocalFree(args);
        std::filesystem::create_directories(app.data);
        app.model=std::make_unique<contour::app::Model>(app.data/L"profiles.contour-library",contour::app::enumerate_devices());
        WNDCLASSW cls{}; cls.lpfnWndProc=window_proc; cls.hInstance=instance; cls.lpszClassName=L"ContourEditor";
        cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassW(&cls);
        const auto dpi=GetDpiForSystem();
        // Parent background erasure must never paint over WebView's child HWND.
        app.window=CreateWindowExW(0,cls.lpszClassName,L"Contour",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,
            MulDiv(1400,static_cast<int>(dpi),96),MulDiv(900,static_cast<int>(dpi),96),nullptr,nullptr,instance,&app);
        if (!app.window) { throw std::runtime_error("Cannot create the Contour window"); }
        BOOL dark=TRUE; DwmSetWindowAttribute(app.window,20,&dark,sizeof(dark));
        ShowWindow(app.window,show); app.initialize(); SetTimer(app.window,1,200,nullptr);
        MSG message{}; while (GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
        app.controller.Reset(); app.view.Reset();
    } catch (const std::exception& e) { MessageBoxW(nullptr,contour::app::wide(e.what()).c_str(),L"Contour",MB_OK|MB_ICONERROR); exit_code=1; }
    CoUninitialize(); return exit_code;
}
