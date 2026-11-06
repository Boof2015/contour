#include <contour/profile_device_session.h>

namespace contour::eq {
namespace {
RunningEqState compare_runtime(const Profile& profile,const PreviewStatus& runtime) {
    if (profile.channels!=runtime.channels) { return RunningEqState::incompatible; }
    ProcessingFingerprint expected;
    try { expected=processing_fingerprint(profile.configuration,runtime.sample_rate,runtime.channels); }
    catch (const std::invalid_argument&) { return RunningEqState::incompatible; }
    if (runtime.state==PreviewState::queued || runtime.evidence==RuntimeEvidence::transitioning) { return RunningEqState::transitioning; }
    if (!runtime.active_fingerprint) { return RunningEqState::unknown; }
    const bool matches=expected==*runtime.active_fingerprint;
    if (runtime.evidence==RuntimeEvidence::loaded) { return matches ? RunningEqState::loaded_match : RunningEqState::loaded_difference; }
    if (runtime.evidence==RuntimeEvidence::processed) { return matches ? RunningEqState::processed_match : RunningEqState::processed_difference; }
    return RunningEqState::unknown;
}
}
ReconciliationReport ProfileDeviceSession::reconcile() {
    require_open(); ReconciliationReport report;
    const auto doc=current();
    if (!doc) { report.library_detail="No profile is assigned to this device"; return report; }
    report.profile_id=doc->id(); report.working=doc->editor()->snapshot(); report.checkpoint=doc->editor()->saved();
    try { workspace_.verify_assignment(); report.library_current=true; }
    catch (const std::exception& e) { report.library_detail=e.what(); return report; }
    refresh();
    if (!endpoint_ || status_.endpoint.state==EndpointState::unavailable || status_.endpoint.state==EndpointState::not_attached) {
        report.saved=SavedSettingsState::unavailable; report.saved_detail=status_.endpoint.detail; return report;
    }
    const auto before=status_.endpoint.runtime; const auto generation=endpoint_->device_change_count();
    try {
        const auto inspected=endpoint_->inspect_saved(); report.windows_saved=inspected.profile;
        report.saved=!inspected.profile ? SavedSettingsState::missing :
            *inspected.profile==report.checkpoint ? SavedSettingsState::matches : SavedSettingsState::differs;
        if (inspected.device_generation!=generation) { throw std::runtime_error("Device changed while reading saved settings"); }
    } catch (const std::invalid_argument& e) { report.saved=SavedSettingsState::invalid; report.saved_detail=e.what(); }
    catch (const std::exception& e) { report.saved=SavedSettingsState::unreadable; report.saved_detail=e.what(); }
    refresh(); report.runtime=status_.endpoint.runtime;
    if (!endpoint_ || endpoint_->device_change_count()!=generation || status_.endpoint.state==EndpointState::unavailable ||
        (before && report.runtime && before->runtime!=report.runtime->runtime)) {
        report.windows_saved.reset(); report.runtime.reset(); report.saved=SavedSettingsState::unavailable;
        report.saved_detail="Device/runtime changed during inspection; inspect again"; return report;
    }
    try {
        workspace_.verify_assignment();
        if (current()!=doc || doc->editor()->snapshot()!=report.working || doc->editor()->saved()!=report.checkpoint) {
            throw std::runtime_error("Editor changed during inspection; inspect again");
        }
    } catch (const std::exception& e) {
        report.library_current=false; report.library_detail=e.what(); report.saved=SavedSettingsState::not_checked;
        report.windows_saved.reset(); report.runtime.reset(); return report;
    }
    if (!report.runtime || !status_is_live(*report.runtime,GetTickCount64())) { report.runtime.reset(); return report; }
    report.runtime_detail=report.runtime->detail;
    try {
        report.running_saved=compare_runtime(report.checkpoint,*report.runtime);
        report.running_working=compare_runtime(report.working->profile(),*report.runtime);
    } catch (const std::exception& e) {
        report.running_saved=report.running_working=RunningEqState::unknown; report.runtime_detail=e.what();
    }
    return report;
}
} // namespace contour::eq
