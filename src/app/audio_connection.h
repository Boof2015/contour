#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace contour::app {
struct ConnectionCheck {
    bool busy=false,confirmed=false,streaming=false;
    std::string detail;
    std::uint64_t finished=0;
};
// A bounded, silent shared-mode stream on an explicitly selected endpoint.
// All WASAPI/COM work stays on its own thread. No tones, loopback capture,
// default-device changes, endpoint writes, or exclusive-mode requests.
class AudioConnection final {
public:
    AudioConnection();
    ~AudioConnection();
    void request(const std::wstring& endpoint,bool force=false);
    ConnectionCheck status(const std::wstring& endpoint) const;
    void suspend(bool value);
private:
    void run();
    ConnectionCheck check(const std::wstring& endpoint);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::wstring,ConnectionCheck> results_;
    std::deque<std::wstring> queue_;
    std::atomic<bool> stopping_{false},suspended_{false};
    std::thread worker_;
};
}
