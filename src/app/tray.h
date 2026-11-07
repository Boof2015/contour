#pragma once
#include "model.h"
#include <WebView2.h>
#include <functional>

namespace contour::app {
// The flyout is a view of the editor's session. Actions return to the editor's
// command queue; this window never owns a second draft or audio connection.
class TrayHost {
public:
    TrayHost(HWND owner,std::filesystem::path assets,std::function<void(const Json&)> action);
    ~TrayHost();
    void initialize(ICoreWebView2Environment* environment);
    bool handle(UINT message,WPARAM w,LPARAM l);
    bool installed() const noexcept { return installed_; }
    void show();
    void hide(bool return_focus=false);
    void update(const Json& state);
private:
    void add_icon();
    void create();
    void send_state();
    void layout(int height);
    void receive(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept;
    void menu();
    static LRESULT CALLBACK proc(HWND,UINT,WPARAM,LPARAM);
    HWND owner_=nullptr,window_=nullptr;
    HICON icon_=nullptr;
    std::filesystem::path assets_;
    std::function<void(const Json&)> action_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> view_;
    Json state_;
    bool installed_=false,ready_=false,requested_=false,light_=false;
    UINT taskbar_created_=0;
    ULONGLONG dismissed_=0;
};
}
