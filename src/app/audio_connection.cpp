#include "audio_connection.h"
#include <contour/live_control.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <memory>
#include <stdexcept>

namespace contour::app {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT result,const char* operation) {
    if (FAILED(result)) { throw std::runtime_error(std::string(operation)+" ("+std::to_string(result)+")"); }
}
struct AudioStop { IAudioClient* client; ~AudioStop(){ client->Stop(); } };
struct Event { HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(handle)CloseHandle(handle);} };
}
AudioConnection::AudioConnection() : worker_([this]{run();}) {}
AudioConnection::~AudioConnection() { stopping_=true;changed_.notify_one();if(worker_.joinable())worker_.join(); }
void AudioConnection::request(const std::wstring& endpoint,bool force) {
    if (endpoint.empty() || suspended_) { return; }
    std::lock_guard lock(mutex_); auto& result=results_[endpoint];
    if (result.busy || (!force && result.finished && GetTickCount64()-result.finished<5000)) { return; }
    result.busy=true; result.detail.clear(); queue_.push_back(endpoint);changed_.notify_one();
}
ConnectionCheck AudioConnection::status(const std::wstring& endpoint) const {
    std::lock_guard lock(mutex_); const auto it=results_.find(endpoint); return it==results_.end()?ConnectionCheck{}:it->second;
}
void AudioConnection::suspend(bool value) {
    suspended_=value;
    if (value) { std::lock_guard lock(mutex_);for(const auto& id:queue_)results_[id].busy=false;queue_.clear(); }
    changed_.notify_one();
}
void AudioConnection::run() {
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    while (!stopping_) {
        std::wstring id;
        {
            std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return stopping_||(!suspended_&&!queue_.empty());});
            if (stopping_) { break; } id=std::move(queue_.front());queue_.pop_front();
        }
        ConnectionCheck result;
        try {
            checked(com,"Cannot initialize the audio connection check");result=check(id);
        } catch(const std::exception& e) { result.detail=e.what(); }
        result.finished=GetTickCount64();
        { std::lock_guard lock(mutex_);results_[id]=std::move(result); }
    }
    if (SUCCEEDED(com)) { CoUninitialize(); }
}
ConnectionCheck AudioConnection::check(const std::wstring& id) {
    using namespace eq;
    ComPtr<IMMDeviceEnumerator> enumerator;
    checked(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Cannot find audio devices");
    ComPtr<IMMDevice> device;checked(enumerator->GetDevice(id.c_str(),&device),"This device is unavailable");
    ComPtr<IPropertyStore> control;
    // Opening this existing Contour context never installs an effect.
    const auto opened=open_control_store(device.Get(),&control);
    if (opened==E_NOTFOUND || opened==E_NOINTERFACE) { return {}; }
    checked(opened,"Cannot read this device's Contour connection");
    ComPtr<IAudioClient> client;checked(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"Cannot open this audio device");
    WAVEFORMATEX* raw=nullptr;checked(client->GetMixFormat(&raw),"Cannot read this device's audio format");
    const std::unique_ptr<WAVEFORMATEX,decltype(&CoTaskMemFree)> format(raw,CoTaskMemFree);
    GUID session{};checked(CoCreateGuid(&session),"Cannot create the audio check session");
    checked(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_NOPERSIST,0,0,format.get(),&session),"Cannot connect to this device. Another app may be using it exclusively");
    Event event;if(!event.handle)throw std::runtime_error("Cannot create the audio check event");
    checked(client->SetEventHandle(event.handle),"Cannot start audio notifications");
    UINT32 capacity=0;checked(client->GetBufferSize(&capacity),"Cannot read the audio buffer size");
    ComPtr<IAudioRenderClient> render;checked(client->GetService(IID_PPV_ARGS(&render)),"Cannot prepare silent audio");
    const auto fill=[&](UINT32 frames) {
        if(!frames)return;
        BYTE* data=nullptr;checked(render->GetBuffer(frames,&data),"Cannot obtain the audio buffer");
        checked(render->ReleaseBuffer(frames,AUDCLNT_BUFFERFLAGS_SILENT),"Cannot submit silent audio");
    };
    fill(capacity);checked(client->Start(),"Cannot start the audio connection check");AudioStop stop{client.Get()};
    ConnectionCheck result;result.streaming=true;const auto started=GetTickCount64();
    // Keep the stream available long enough for the app -> APO -> app
    // acknowledgement, including a newly created runtime's first heartbeat.
    while(!stopping_&&!suspended_&&GetTickCount64()-started<1500) {
        if(WaitForSingleObject(event.handle,20)==WAIT_OBJECT_0) {
            UINT32 padding=0;checked(client->GetCurrentPadding(&padding),"The audio device disconnected");
            if(padding>capacity)throw std::runtime_error("Invalid audio buffer state");
            fill(capacity-padding);
        }
        const auto bytes=read_control_value(*control.Get(),preview_status_key,max_status_bytes);
        if(bytes){
            const auto status=decode_status(*bytes);
            if(status.heartbeat_ms>=started&&status_is_live(status,GetTickCount64())){
                result.confirmed=true;result.engine_path.clear();
                try {
                    const auto identity=read_control_value(*control.Get(),engine_identity_key,max_engine_identity_bytes);
                    if(identity){const auto engine=decode_engine_identity(*identity);if(engine.runtime==status.runtime)result.engine_path=engine.module_path;}
                } catch (...) {} // Older engines have no build evidence.
            }
        }
    }
    if(!result.confirmed&&!stopping_&&!suspended_)result.detail="Contour did not respond on this device. Check that audio enhancements are enabled, or check the installation again.";
    return result;
}
}
