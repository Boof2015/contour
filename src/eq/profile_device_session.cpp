#include <contour/profile_device_session.h>

namespace contour::eq {

ProfileDeviceSession::ProfileDeviceSession(ProfileLibrary& library,std::wstring endpoint_id,IMMDeviceEnumerator* enumerator)
    : workspace_(library,endpoint_id),endpoint_id_(std::move(endpoint_id)),enumerator_(enumerator) {
    if (endpoint_id_.empty()) { throw std::invalid_argument("An explicit endpoint ID is required"); }
    pending();
}
ProfileDeviceSession::~ProfileDeviceSession()=default;
void ProfileDeviceSession::require_open() const {
    if (close_status_.state!=CloseState::open) { throw std::logic_error("Cancel the close operation before editing or publishing"); }
}
void ProfileDeviceSession::pending() {
    copied_.reset(); copied_runtime_=GUID_NULL; copied_generation_=0;
    status_.copy=DeviceCopyState::pending; status_.copy_detail="Saved profile has not been confirmed on this device";
}
void ProfileDeviceSession::bind() {
    const auto doc=current(); const auto editor=closing_ ? closing_->target : (doc ? doc->editor() : nullptr);
    if (endpoint_ && bound_editor_==editor) { return; }
    if (endpoint_ && editor) { endpoint_->rebind(*editor); }
    else { endpoint_.reset(); }
    bound_editor_=editor; pending();
    status_.preview_detail.clear(); status_.endpoint={};
    if (editor && !endpoint_) { endpoint_=std::make_unique<EndpointSession>(*editor,endpoint_id_,enumerator_.Get()); }
}
const ProfileDeviceStatus& ProfileDeviceSession::refresh() {
    if (close_status_.state==CloseState::closed) { return status_; }
    try {
        bind();
        if (!endpoint_) { return status_; }
        status_.endpoint=endpoint_->refresh();
        if (copied_ && (*copied_!=bound_editor_->saved() || !status_.endpoint.runtime ||
            status_.endpoint.runtime->runtime!=copied_runtime_ || endpoint_->device_change_count()!=copied_generation_)) { pending(); }
    } catch (const std::exception& e) {
        endpoint_.reset(); pending();
        status_.endpoint={EndpointState::error,0,{},PreviewFeedback::disconnected,e.what()};
    }
    return status_;
}
SelectionResult ProfileDeviceSession::select(std::string_view id,std::optional<UnsavedChoice> choice) {
    require_open();
    const auto result=workspace_.select(id,choice);
    if (result==SelectionResult::selected) { synchronize(); }
    return result;
}
const ProfileDeviceStatus& ProfileDeviceSession::save() {
    require_open();
    workspace_.save(); return synchronize();
}
const ProfileDeviceStatus& ProfileDeviceSession::revert() {
    require_open();
    workspace_.revert(); return synchronize();
}
ProfileEntry ProfileDeviceSession::save_as(std::string name) {
    require_open();
    auto entry=workspace_.save_as(std::move(name)); synchronize(); return entry;
}
const ProfileDeviceStatus& ProfileDeviceSession::preview() {
    require_open(); return preview_current();
}
ProfileEntry ProfileDeviceSession::duplicate(std::string name) {
    require_open(); return workspace_.duplicate(std::move(name));
}
void ProfileDeviceSession::rename(std::string name) {
    require_open(); workspace_.rename(std::move(name));
}
const ProfileDeviceStatus& ProfileDeviceSession::preview_current() {
    refresh(); status_.preview_detail.clear();
    if (!endpoint_) { status_.preview_detail="No profile is bound to the selected device"; return status_; }
    try { workspace_.verify_assignment(); }
    catch (const std::exception& e) {
        pending(); bound_editor_->clear_applied(); status_.endpoint.preview=PreviewFeedback::disconnected;
        status_.preview_detail=e.what(); return status_;
    }
    try { device_touched_=true; endpoint_->preview(); }
    catch (const std::exception& e) { status_.preview_detail=e.what(); }
    return refresh();
}
const ProfileDeviceStatus& ProfileDeviceSession::synchronize() {
    require_open(); return synchronize_current();
}
const ProfileDeviceStatus& ProfileDeviceSession::synchronize_current() {
    refresh(); status_.preview_detail.clear();
    if (!endpoint_ || !bound_editor_) { return status_; }
    // Stage the value before device I/O. The library checkpoint never changes
    // as a side effect of Commit, readback, a device notification, or a retry.
    auto saved=bound_editor_->saved();
    pending();
    try {
        workspace_.verify_assignment();
        device_touched_=true;
        const auto receipt=endpoint_->persist(saved);
        status_.endpoint=endpoint_->refresh();
        if (!status_.endpoint.runtime || status_.endpoint.runtime->runtime!=receipt.runtime ||
            endpoint_->device_change_count()!=receipt.device_generation ||
            (status_.endpoint.state!=EndpointState::ready && status_.endpoint.state!=EndpointState::incompatible)) {
            throw std::runtime_error("Device changed after saving; retry its saved profile copy");
        }
        copied_=std::move(saved); copied_runtime_=receipt.runtime; copied_generation_=receipt.device_generation;
        status_.copy=DeviceCopyState::current; status_.copy_detail.clear();
    } catch (const std::exception& e) {
        status_.copy=DeviceCopyState::failed; status_.copy_detail=e.what();
        status_.endpoint=endpoint_->refresh(); return status_;
    }
    return preview_current();
}
} // namespace contour::eq
