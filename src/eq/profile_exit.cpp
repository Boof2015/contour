#include <contour/profile_device_session.h>

namespace contour::eq {

void ProfileDeviceSession::verify_close_source() const {
    if (!closing_) { throw std::logic_error("No resolved close operation"); }
    const auto& attempt=*closing_;
    if (current()!=attempt.source || (attempt.source && (attempt.source->editor()!=attempt.editor ||
        attempt.editor->snapshot()!=attempt.draft || attempt.editor->saved()!=attempt.target->saved()))) {
        throw std::runtime_error("Edits changed while closing; cancel close and review the new changes");
    }
}
const ProfileCloseStatus& ProfileDeviceSession::request_close(std::optional<UnsavedChoice> choice) {
    if (close_status_.state==CloseState::closed) { return close_status_; }
    if (choice && *choice!=UnsavedChoice::save && *choice!=UnsavedChoice::discard && *choice!=UnsavedChoice::cancel) {
        throw std::invalid_argument("Invalid close decision");
    }
    if (choice==UnsavedChoice::cancel) { return cancel_close(); }
    if (closing_) {
        if (choice && *choice!=closing_->decision) { throw std::logic_error("Cancel the existing close operation before changing its decision"); }
        return poll_close();
    }
    const auto source=current();
    if (source && source->editor()->dirty() && !choice) {
        close_status_={CloseState::needs_decision,false,"Save changes before closing?"}; return close_status_;
    }
    try {
        CloseAttempt attempt; attempt.source=source; attempt.decision=choice.value_or(UnsavedChoice::save);
        if (source) {
            attempt.editor=source->editor(); attempt.draft=attempt.editor->snapshot();
            const bool dirty=attempt.editor->dirty();
            attempt.requires_device=device_touched_ || (dirty && attempt.decision==UnsavedChoice::save);
            // Don't save has a separate audio editor so cancellation/failure
            // cannot destroy the original unsaved draft or its undo history.
            attempt.target=dirty && attempt.decision==UnsavedChoice::discard ?
                std::make_shared<EditSession>(attempt.editor->saved()) : attempt.editor;
            if (dirty && attempt.decision==UnsavedChoice::save) { workspace_.save(); }
        }
        closing_=std::move(attempt);
        close_status_={CloseState::needs_attention,false,{}};
        return retry_close();
    } catch (const std::exception& e) {
        close_status_={CloseState::needs_attention,false,e.what()}; return close_status_;
    }
}
const ProfileCloseStatus& ProfileDeviceSession::retry_close() {
    if (!closing_ || close_status_.state==CloseState::closed) { return close_status_; }
    if (close_status_.state==CloseState::waiting || close_status_.state==CloseState::ready) { return poll_close(); }
    try {
        verify_close_source();
        if (!closing_->requires_device) {
            close_status_={CloseState::ready,status_.copy!=DeviceCopyState::current,
                           "Editor can close; no device update is required by this session"}; return close_status_;
        }
        workspace_.verify_assignment();
        closing_->started_ms=GetTickCount64();
        refresh();
        // A clean, already confirmed profile can close without another write.
        if (!(status_.copy==DeviceCopyState::current && status_.endpoint.state==EndpointState::ready &&
              status_.endpoint.preview==PreviewFeedback::applied && closing_->target->applied()==closing_->target->snapshot())) {
            synchronize_current();
        }
        close_status_={CloseState::waiting,true,"Waiting for the final EQ to be confirmed by the audio runtime"};
        return poll_close();
    } catch (const std::exception& e) {
        close_status_={CloseState::needs_attention,true,e.what()}; return close_status_;
    }
}
const ProfileCloseStatus& ProfileDeviceSession::poll_close() {
    if (!closing_ || (close_status_.state!=CloseState::waiting && close_status_.state!=CloseState::ready)) { return close_status_; }
    try {
        verify_close_source();
        if (!closing_->requires_device) { return close_status_; }
        workspace_.verify_assignment(); refresh();
        if (status_.copy!=DeviceCopyState::current) {
            throw std::runtime_error(status_.copy_detail.empty() ? "Final device settings have not been confirmed; retry or leave the update pending" : status_.copy_detail);
        }
        if (!status_.preview_detail.empty()) { throw std::runtime_error(status_.preview_detail); }
        if (status_.endpoint.state!=EndpointState::ready || status_.endpoint.preview==PreviewFeedback::rejected ||
            status_.endpoint.preview==PreviewFeedback::disconnected) {
            throw std::runtime_error("Final audio update was not confirmed; retry or leave the update pending");
        }
        if (status_.endpoint.preview==PreviewFeedback::applied && closing_->target->applied()==closing_->target->snapshot()) {
            close_status_={CloseState::ready,false,{}}; return close_status_;
        }
        if (GetTickCount64()-closing_->started_ms>=close_timeout_ms) {
            throw std::runtime_error("Audio did not acknowledge the final EQ in time; retry, cancel close, or leave the update pending");
        }
        close_status_={CloseState::waiting,true,"Waiting for the final EQ to be confirmed by the audio runtime"};
    } catch (const std::exception& e) { close_status_={CloseState::needs_attention,true,e.what()}; }
    return close_status_;
}
const ProfileCloseStatus& ProfileDeviceSession::cancel_close() {
    if (close_status_.state==CloseState::closed) { return close_status_; }
    const bool restored_binding=closing_ && closing_->editor!=closing_->target && bound_editor_==closing_->target;
    if (restored_binding) {
        if (endpoint_) { endpoint_->rebind(*closing_->editor); }
        bound_editor_=closing_->editor;
        status_.endpoint.preview=PreviewFeedback::disconnected;
    }
    closing_.reset();
    close_status_={CloseState::open,false,restored_binding ?
        "Close cancelled; the saved EQ may finish applying. The draft and undo history are retained; preview to audition it again" : ""};
    return close_status_;
}
const ProfileCloseStatus& ProfileDeviceSession::finish_close(CloseCompletion completion) {
    if (completion!=CloseCompletion::confirmed && completion!=CloseCompletion::leave_device_pending) { throw std::invalid_argument("Invalid close completion"); }
    if (close_status_.state==CloseState::closed) { return close_status_; }
    poll_close();
    if (!closing_ || (close_status_.state!=CloseState::ready &&
        !(completion==CloseCompletion::leave_device_pending && close_status_.state==CloseState::needs_attention))) { return close_status_; }
    try {
        // Even explicit pending completion cannot discard edits made after the
        // user's decision or bypass a failed library Save (which has no attempt).
        verify_close_source();
        const bool pending_device=close_status_.device_pending;
        workspace_.close(closing_->decision==UnsavedChoice::discard ? std::optional{UnsavedChoice::discard} : std::nullopt);
        endpoint_.reset(); bound_editor_.reset(); closing_.reset();
        status_.endpoint={}; status_.preview_detail.clear();
        close_status_={CloseState::closed,pending_device,pending_device ?
            "Editor closed; device settings or audible application remain unconfirmed" : ""};
    } catch (const std::exception& e) { close_status_={CloseState::needs_attention,true,e.what()}; }
    return close_status_;
}
} // namespace contour::eq
