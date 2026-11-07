// Exercise the real host's visibility/single-instance/session messages against
// an unassigned, isolated library. Never log off or shut down Windows, register
// actual startup, install an APO, or change a real output's EQ.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Process {
    PROCESS_INFORMATION info{};
    Process(const std::filesystem::path& exe,const std::filesystem::path& data,bool startup) {
        auto command=L"\""+exe.wstring()+L"\" --data-dir \""+data.wstring()+L"\""+(startup?L" --startup":L"");
        STARTUPINFOW start{sizeof(start)};
        require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&info)!=FALSE,"Launch isolated editor");
    }
    ~Process(){if(info.hProcess){if(WaitForSingleObject(info.hProcess,0)==WAIT_TIMEOUT)TerminateProcess(info.hProcess,1);CloseHandle(info.hThread);CloseHandle(info.hProcess);}}
    bool running() const{return WaitForSingleObject(info.hProcess,0)==WAIT_TIMEOUT;}
};
struct WindowSearch{DWORD process;HWND window=nullptr;};
HWND find_window(DWORD process){
    WindowSearch search{process};
    EnumWindows([](HWND window,LPARAM raw)->BOOL{
        auto& s=*reinterpret_cast<WindowSearch*>(raw);DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        wchar_t name[64]{};GetClassNameW(window,name,64);
        if(pid==s.process&&std::wstring_view(name)==L"ContourEditor"){s.window=window;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));return search.window;
}
template<class Predicate> void wait_for(Predicate test,const char* message){
    const auto deadline=GetTickCount64()+10000;
    while(!test()){require(GetTickCount64()<deadline,message);Sleep(25);}
}
void send_session(HWND window,UINT message,WPARAM ended){
    DWORD_PTR result=0;require(SendMessageTimeoutW(window,message,ended,ENDSESSION_LOGOFF,SMTO_ABORTIFHUNG,4000,&result)!=0,"Isolated session message timed out");
    if(message==WM_QUERYENDSESSION)require(result==TRUE,"Clean editor blocked sign-out");
}
void exercise(const std::filesystem::path& exe,const std::filesystem::path& data){
    std::filesystem::create_directories(data);
    std::ofstream(data/L"profiles.contour-library.settings")<<R"({"version":1,"eqEnabled":true,"startInTray":true,"closeToTray":true})";
    Process editor(exe,data,true);HWND window=nullptr;
    wait_for([&]{window=find_window(editor.info.dwProcessId);return window!=nullptr;},"No isolated editor window");
    // The caption becomes custom once the WebView and native tray initialize.
    wait_for([&]{return FindWindowExW(window,nullptr,L"Chrome_WidgetWin_0",nullptr)!=nullptr||FindWindowExW(window,nullptr,L"Chrome_WidgetWin_1",nullptr)!=nullptr;},"WebView did not initialize");
    Sleep(500);require(editor.running()&&!IsWindowVisible(window),"Start in tray displayed the editor");
    {Process duplicate(exe,data,true);wait_for([&]{return !duplicate.running();},"Second startup instance remained alive");}
    require(!IsWindowVisible(window),"Startup launch raised the existing window");
    {Process explicit_open(exe,data,false);wait_for([&]{return !explicit_open.running();},"Explicit second instance remained alive");}
    wait_for([&]{return IsWindowVisible(window)!=FALSE;},"Explicit launch did not reveal the existing editor");
    send_session(window,WM_QUERYENDSESSION,0);send_session(window,WM_ENDSESSION,FALSE);
    require(editor.running()&&IsWindowVisible(window),"Cancelled sign-out closed or hid the editor");
    send_session(window,WM_QUERYENDSESSION,0);send_session(window,WM_ENDSESSION,TRUE);
    wait_for([&]{return !editor.running();},"Confirmed sign-out was routed to close-to-tray or waited for audio");
}
}
int wmain(int argc,wchar_t** argv){
    try {
        require(argc==3,"Usage: lifecycle-tests contour.exe test-directory");
        const auto dir=std::filesystem::absolute(argv[2])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        exercise(std::filesystem::absolute(argv[1]),dir);
        std::cout<<"PASS start hidden; automatic relaunch stays hidden; explicit relaunch restores; cancelled and confirmed session-end messages\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
