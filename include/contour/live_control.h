#pragma once

#include <contour/edit_session.h>
#include <contour/live_eq.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <array>

namespace contour::eq {

// Contour-owned Windows effects settings context. Installation must provision
// this context; opening it never creates keys or changes device attachment.
inline constexpr GUID control_context = {0x8b5699ab,0xdbc3,0x4ff7,{0x82,0xed,0x8f,0xbc,0xbf,0x7c,0x8c,0xf4}};
inline constexpr PROPERTYKEY preview_request_key{control_context,1};
inline constexpr PROPERTYKEY preview_status_key{control_context,2};
// Property 3 is installer metadata in Default. Property 4 is one complete
// native profile in User; it is independent of volatile preview requests.
inline constexpr PROPERTYKEY saved_profile_key{control_context,4};
inline constexpr std::size_t max_saved_profile_bytes = 1024*1024;
inline constexpr std::size_t max_request_bytes = 1024*1024+52;
inline constexpr std::size_t max_status_bytes = 76+2048+8+32;
inline constexpr std::uint64_t control_timeout_ms = 2000;

enum class PreviewState : std::uint32_t { ready, queued, applied, rejected, stopped };
using ProcessingFingerprint=std::array<unsigned char,32>;
enum class RuntimeEvidence : std::uint32_t { unknown, loaded, processed, transitioning };
// Control thread only. SHA-256 of actual format and ordered compiled sections;
// excludes display names and the profile's unrelated validation sample rate.
ProcessingFingerprint processing_fingerprint(const Configuration& configuration,double rate,std::size_t channels);
struct PreviewRequest {
    GUID runtime{}, session{};
    std::uint64_t revision = 0;
    std::string profile;
};
struct PreviewStatus {
    GUID runtime{}, session{};
    std::uint64_t revision = 0;
    PreviewState state = PreviewState::ready;
    double sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t heartbeat_ms = 0;
    std::string detail;
    RuntimeEvidence evidence=RuntimeEvidence::unknown;
    std::optional<ProcessingFingerprint> active_fingerprint;
};

// Bounded, versioned binary envelopes; profile contents use the native parser.
// Header decoding is separate so invalid profiles can receive a correlated
// rejection. These functions and every class below are non-real-time only.
std::string encode_request(const PreviewRequest& request);
PreviewRequest decode_request(std::string_view bytes);
std::string encode_status(const PreviewStatus& status);
PreviewStatus decode_status(std::string_view bytes);
bool status_is_live(const PreviewStatus& status, std::uint64_t now) noexcept;

HRESULT open_control_store(IMMDevice* endpoint, IPropertyStore** store) noexcept;
HRESULT open_control_store(IMMDeviceCollection* devices, IPropertyStore** store) noexcept;
HRESULT open_saved_store(IMMDevice* endpoint, DWORD access, IPropertyStore** store) noexcept;
HRESULT open_saved_store(IMMDeviceCollection* devices, DWORD access, IPropertyStore** store) noexcept;
std::optional<std::string> read_control_value(IPropertyStore& store, REFPROPERTYKEY key, std::size_t limit);
void write_control_value(IPropertyStore& store, REFPROPERTYKEY key, std::string_view bytes);
// Missing is distinct from malformed/unreadable (which throws). Save performs
// validation, a single complete property write, Commit, and exact readback
// before moving the editor checkpoint. It does not publish a preview or claim
// audio application. Call with a freshly opened User store on a control thread.
std::optional<Profile> read_saved_profile(IPropertyStore& store);
void write_saved_profile(IPropertyStore& store, const Profile& profile);
void save_profile_settings(EditSession& editor, IPropertyStore& store);

// Sole publisher to a locked SmoothEq. One instance per LockForProcess creates
// a new identity. Requests addressed to an older runtime are ignored. It keeps
// only the latest requested revision; coalesced intermediate edits need no ack.
class PreviewProcessor final {
public:
    PreviewProcessor(SmoothEq& runtime, double rate, std::size_t channels, std::string startup_detail = {},
                     std::optional<ProcessingFingerprint> initial_fingerprint = {});
    PreviewProcessor(const PreviewProcessor&) = delete;
    PreviewProcessor& operator=(const PreviewProcessor&) = delete;
    void submit(std::string_view bytes);
    const PreviewStatus& poll();
    const PreviewStatus& stop() noexcept;
private:
    SmoothEq& runtime_;
    PreviewStatus status_;
    std::uint64_t bank_revision_ = 0;
    GUID last_session_{};
    std::uint64_t last_request_revision_ = 0;
    std::uint64_t active_revision_=0;
    std::optional<std::uint64_t> pending_revision_;
    std::optional<ProcessingFingerprint> active_fingerprint_,pending_fingerprint_;
};

enum class PreviewFeedback { disconnected, ready, waiting, applied, rejected };
struct PreviewReply {
    PreviewFeedback feedback = PreviewFeedback::disconnected;
    std::string detail;
};

// Single-editor, single-endpoint development bridge. Store access belongs to
// the caller's COM-initialized control thread. A library Save still goes only
// through EditSession; preview requests never overwrite the saved profile.
class LivePreview final {
public:
    LivePreview(EditSession& editor, IPropertyStore* store, std::optional<bool> bypass_override = {});
    LivePreview(const LivePreview&) = delete;
    LivePreview& operator=(const LivePreview&) = delete;
    PreviewStatus connect(); // Explicitly bind/rebind; never reapplies edits.
    void submit();
    PreviewReply poll();
private:
    std::optional<PreviewStatus> read_status();
    void disconnect() noexcept;
    EditSession& editor_;
    std::optional<bool> bypass_override_;
    Microsoft::WRL::ComPtr<IPropertyStore> store_;
    EditSession::Snapshot pending_;
    GUID runtime_id_{}, session_id_{};
};

} // namespace contour::eq
