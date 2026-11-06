// Uses the same bounded silent stream as the editor, without changing EQ.
#include "audio_connection.h"
#include "setup.h"
#include <iostream>
#include <mmdeviceapi.h>
#include <wrl/client.h>

int wmain(int argc,wchar_t** argv) {
    using namespace contour::app;
    if(argc!=3 || !valid_setup_endpoint(argv[2]) ||
       (std::wstring_view(argv[1])!=L"present" && std::wstring_view(argv[1])!=L"absent")) {
        std::cerr<<"Usage: contour-endpoint-check <present|absent> <playback-endpoint-ID>\n";return 2;
    }
    try {
        const bool expected=std::wstring_view(argv[1])==L"present";
        if(!expected) {
            const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            if(FAILED(com))throw std::runtime_error("Cannot initialize device inspection");
            bool inactive=false;
            {
                Microsoft::WRL::ComPtr<IMMDeviceEnumerator> devices;
                Microsoft::WRL::ComPtr<IMMDevice> device;
                DWORD state=0;
                const auto created=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices));
                const auto found=SUCCEEDED(created)?devices->GetDevice(argv[2],&device):created;
                const auto read=SUCCEEDED(found)?device->GetState(&state):found;
                inactive=SUCCEEDED(read)&&state!=DEVICE_STATE_ACTIVE;
            }
            CoUninitialize();
            if(inactive){std::cout<<"{\"verified\":true,\"inactive\":true}\n";return 0;}
        }
        AudioConnection connection;connection.request(argv[2],true);
        const auto start=GetTickCount64();
        while(GetTickCount64()-start<10000) {
            const auto result=connection.status(argv[2]);
            if(!result.busy&&result.finished) {
                const bool verified=result.streaming&&result.confirmed==expected;
                std::cout<<Json({{"verified",verified},{"streaming",result.streaming},
                    {"confirmed",result.confirmed},{"detail",result.detail}}).dump()<<'\n';
                return verified?0:1;
            }
            Sleep(20);
        }
        throw std::runtime_error("Device verification timed out");
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
