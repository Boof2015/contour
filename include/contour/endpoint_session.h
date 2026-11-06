#pragma once

#include <contour/live_control.h>
#include <memory>

namespace contour::eq {

enum class EndpointState { unavailable, not_attached, waiting_for_audio, incompatible, ready, error };
const char* endpoint_state_name(EndpointState state) noexcept;

struct EndpointStatus {
    EndpointState state = EndpointState::unavailable;
    DWORD device_state = 0;
    std::optional<PreviewStatus> runtime;
    PreviewFeedback preview = PreviewFeedback::disconnected;
    std::string detail;
};

struct EndpointCopyReceipt {
    GUID runtime{};
    std::uint64_t device_generation=0;
};
struct EndpointSavedSnapshot {
    std::optional<Profile> profile;
    std::uint64_t device_generation=0;
};

// App-side controller for one explicit, opaque endpoint ID. The editor and COM
// apartment must outlive it. Call refresh periodically (e.g. every 100 ms) on
// that same control thread; notification callbacks only invalidate an epoch.
// Reconnection never replaces the draft/checkpoint/history, sends a preview,
// changes the default device, starts a stream, or installs effects.
class EndpointSession final {
public:
    EndpointSession(EditSession& editor, std::wstring endpoint_id, IMMDeviceEnumerator* enumerator = nullptr);
    ~EndpointSession();
    EndpointSession(const EndpointSession&) = delete;
    EndpointSession& operator=(const EndpointSession&) = delete;

    const std::wstring& endpoint_id() const noexcept;
    std::uint64_t device_change_count() const noexcept;
    const EndpointStatus& refresh();
    // Switch editors while preserving the endpoint observer and retired-runtime
    // history. The new editor must outlive this binding; no audio is published.
    void rebind(EditSession& editor) noexcept;
    // Application master state is applied to device payloads, never to editor
    // snapshots, checkpoints or history. A change retires old acknowledgements.
    void set_bypass_override(std::optional<bool> bypass) noexcept;
    // Recheck device/runtime and compile at its actual rate before either
    // operation. Channel-count mismatch blocks both; no implicit remapping.
    void preview();
    void save();
    // Persist a validated device copy without advancing any editor checkpoint
    // or publishing audio. Used when the library owns the saved checkpoint.
    // Validates this profile (not an unrelated working draft) at the live format.
    EndpointCopyReceipt persist(const Profile& profile);
    // Persist the next-start copy when the audio runtime is idle/disconnected.
    // Uses the existing settings store only; does not attach or create a device.
    void persist_inactive(const Profile& profile);
    // Opens User settings read-only, independently of runtime availability.
    // Missing value is distinct from malformed/unreadable settings (throws).
    EndpointSavedSnapshot inspect_saved();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace contour::eq
