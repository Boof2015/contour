#pragma once
#include <contour/profile_library.h>
#include <contour/endpoint_session.h>

namespace contour::eq {

// Saved in the library and copied to Windows are independent facts. "current"
// means the last explicit copy passed Commit/readback in this device epoch,
// not that audio has applied it or that another process cannot change it later.
enum class DeviceCopyState { pending, current, failed };
struct ProfileDeviceStatus {
    EndpointStatus endpoint;
    DeviceCopyState copy=DeviceCopyState::pending;
    std::string copy_detail;
    std::string preview_detail;
};

enum class SavedSettingsState { not_checked, unavailable, missing, matches, differs, invalid, unreadable };
enum class RunningEqState { unavailable, unknown, loaded_match, loaded_difference, processed_match, processed_difference, transitioning, incompatible };
struct ReconciliationReport {
    std::string profile_id;
    EditSession::Snapshot working;
    Profile checkpoint;
    bool library_current=false;
    SavedSettingsState saved=SavedSettingsState::not_checked;
    RunningEqState running_saved=RunningEqState::unavailable, running_working=RunningEqState::unavailable;
    std::optional<Profile> windows_saved;
    std::optional<PreviewStatus> runtime;
    std::string library_detail,saved_detail,runtime_detail;
};

enum class CloseState { open, needs_decision, waiting, ready, needs_attention, closed };
enum class CloseCompletion { confirmed, leave_device_pending };
struct ProfileCloseStatus {
    CloseState state=CloseState::open;
    bool device_pending=false;
    std::string detail;
};

// Single selected device, serialized COM control thread, library must outlive
// this controller. Construction/refresh never write settings or play audio.
// Construction restores the exact endpoint's remembered profile from the
// library without applying it. Cross-device propagation is not implicit.
class ProfileDeviceSession final {
public:
    ProfileDeviceSession(ProfileLibrary& library,std::wstring endpoint_id,IMMDeviceEnumerator* enumerator=nullptr);
    ~ProfileDeviceSession();
    ProfileDeviceSession(const ProfileDeviceSession&)=delete;
    ProfileDeviceSession& operator=(const ProfileDeviceSession&)=delete;

    std::shared_ptr<const ProfileDocument> current() const noexcept { return workspace_.current(); }
    const std::wstring& endpoint_id() const noexcept { return endpoint_id_; }
    const ProfileDeviceStatus& status() const noexcept { return status_; }
    const ProfileDeviceStatus& refresh();
    // Read-only point-in-time comparison for startup/reconnection. Never
    // changes the library, checkpoints, undo, device settings, or preview.
    // Content evidence does not adopt another editor's acknowledgement.
    ReconciliationReport reconcile();
    // Local validation/storage failures throw before touching the device.
    // After a successful library operation, device failures are returned in
    // status(), leaving the saved library authoritative and retryable.
    SelectionResult select(std::string_view id,std::optional<UnsavedChoice> choice={});
    const ProfileDeviceStatus& save();
    const ProfileDeviceStatus& revert();
    ProfileEntry save_as(std::string name);
    ProfileEntry duplicate(std::string name);
    void rename(std::string name);
    // Edits/undo remain on current()->editor(); preview auditions the working
    // draft without saving. Synchronize explicitly retries the saved device
    // copy, then previews the current draft; refresh/reconnect never retry it.
    const ProfileDeviceStatus& preview();
    const ProfileDeviceStatus& synchronize();
    // Nonblocking exit handshake. Save commits the library first; Don't save
    // retains the draft until finish_close while a separate editor restores
    // the checkpoint. Poll never retries writes or closes the editor itself.
    static constexpr std::uint64_t close_timeout_ms=2000;
    const ProfileCloseStatus& close_status() const noexcept { return close_status_; }
    const ProfileCloseStatus& request_close(std::optional<UnsavedChoice> choice={});
    const ProfileCloseStatus& poll_close();
    const ProfileCloseStatus& retry_close();
    const ProfileCloseStatus& cancel_close();
    // Default completion requires readiness. Leaving a device update pending
    // is an explicit UI decision after needs_attention, never an automatic
    // fallback; it cannot bypass failed library Save or newly changed edits.
    const ProfileCloseStatus& finish_close(CloseCompletion completion=CloseCompletion::confirmed);
private:
    struct CloseAttempt {
        std::shared_ptr<const ProfileDocument> source;
        std::shared_ptr<EditSession> editor;
        EditSession::Snapshot draft;
        std::shared_ptr<EditSession> target;
        UnsavedChoice decision=UnsavedChoice::save;
        bool requires_device=false;
        std::uint64_t started_ms=0;
    };
    void require_open() const;
    void verify_close_source() const;
    const ProfileDeviceStatus& preview_current();
    const ProfileDeviceStatus& synchronize_current();
    void bind();
    void pending();
    ProfileWorkspace workspace_;
    std::wstring endpoint_id_;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    // Keep the editor alive until after the EndpointSession is destroyed,
    // including Save as, which detaches the original document's editor.
    std::shared_ptr<EditSession> bound_editor_;
    std::unique_ptr<EndpointSession> endpoint_;
    std::optional<Profile> copied_;
    GUID copied_runtime_{};
    std::uint64_t copied_generation_=0;
    ProfileDeviceStatus status_;
    bool device_touched_=false;
    std::optional<CloseAttempt> closing_;
    ProfileCloseStatus close_status_;
};
} // namespace contour::eq
