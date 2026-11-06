#include <contour/profile_device_session.h>

namespace contour::eq {

ProfileDeviceSession::ProfileDeviceSession(ProfileLibrary& library,std::wstring endpoint_id,IMMDeviceEnumerator* enumerator,std::optional<bool> bypass_override)
    : bypass_override_(bypass_override),workspace_(library,endpoint_id),endpoint_id_(std::move(endpoint_id)),enumerator_(enumerator) {
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
    if (endpoint_) { endpoint_->set_bypass_override(bypass_override_); }
}
Profile ProfileDeviceSession::processing(Profile profile) const {
    if (bypass_override_) { profile.configuration.bypass=*bypass_override_; }
    return profile;
}
const ProfileDeviceStatus& ProfileDeviceSession::set_bypass_override(bool bypass) {
    require_open(); bypass_override_=bypass; pending();
    if (endpoint_) { endpoint_->set_bypass_override(bypass); }
    refresh();
    if (!endpoint_ || !bound_editor_ || status_.endpoint.state==EndpointState::not_attached) { return status_; }
    return synchronize();
}
const ProfileDeviceStatus& ProfileDeviceSession::refresh() {
    if (close_status_.state==CloseState::closed) { return status_; }
    try {
        bind();
        if (!endpoint_) { return status_; }
        status_.endpoint=endpoint_->refresh();
        if (copied_ && (*copied_!=bound_editor_->saved() ||
            (copied_runtime_!=GUID_NULL && (!status_.endpoint.runtime || status_.endpoint.runtime->runtime!=copied_runtime_)) ||
            endpoint_->device_change_count()!=copied_generation_)) { pending(); }
        if (bound_editor_->applied()==bound_editor_->snapshot() && status_.endpoint.preview==PreviewFeedback::applied) { wake_requested_=false; }
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
    require_open(); delivery_requested_=wake_requested_=true; return preview_current();
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
    if (status_.endpoint.state==EndpointState::waiting_for_audio || status_.endpoint.state==EndpointState::unavailable) { return status_; }
    try { device_touched_=true; endpoint_->preview(); }
    catch (const std::exception& e) { status_.preview_detail=e.what(); }
    return refresh();
}
const ProfileDeviceStatus& ProfileDeviceSession::synchronize() {
    require_open(); delivery_requested_=wake_requested_=true; return synchronize_current();
}
const ProfileDeviceStatus& ProfileDeviceSession::deliver_pending() {
    refresh();
    if (!delivery_requested_ || close_status_.state!=CloseState::open || !endpoint_ || !bound_editor_) { return status_; }
    if (status_.endpoint.state!=EndpointState::ready || status_.endpoint.preview==PreviewFeedback::rejected) { return status_; }
    // A connected but busy engine will acknowledge the queued request. Retry
    // errors at a bounded rate; never continuously requeue an in-flight update.
    if (status_.endpoint.preview==PreviewFeedback::waiting) { return status_; }
    if (status_.copy==DeviceCopyState::failed && GetTickCount64()-last_delivery_<1000) { return status_; }
    if (bound_editor_->applied()==bound_editor_->snapshot() && status_.copy==DeviceCopyState::current) { return status_; }
    last_delivery_=GetTickCount64();
    return status_.copy==DeviceCopyState::current ? preview_current() : synchronize_current();
}
const ProfileDeviceStatus& ProfileDeviceSession::synchronize_current() {
    wake_requested_=true;
    refresh(); status_.preview_detail.clear();
    if (!endpoint_ || !bound_editor_) { return status_; }
    // Stage the value before device I/O. The library checkpoint never changes
    // as a side effect of Commit, readback, a device notification, or a retry.
    auto saved=bound_editor_->saved();
    pending();
    try {
        workspace_.verify_assignment();
        device_touched_=true;
        if (status_.endpoint.state==EndpointState::waiting_for_audio || status_.endpoint.state==EndpointState::unavailable) {
            const auto generation=endpoint_->device_change_count();
            endpoint_->persist_inactive(saved);
            if (endpoint_->device_change_count()!=generation) { throw std::runtime_error("Device changed while saving EQ"); }
            copied_=std::move(saved); copied_runtime_=GUID_NULL; copied_generation_=generation;
            status_.copy=DeviceCopyState::current; status_.copy_detail.clear();
            return status_;
        }
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
