#include <contour/edit_session.h>
#include <limits>
#include <stdexcept>
#include <utility>

namespace contour::eq {

EditSnapshot::EditSnapshot(std::shared_ptr<const Identity> owner, Profile profile, std::uint64_t revision)
    : owner_(std::move(owner)), profile_(std::move(profile)), revision_(revision) {}

EditSession::EditSession(Profile saved) : identity_(std::make_shared<EditSnapshot::Identity>()) {
    working_ = make_snapshot(std::move(saved));
    saved_ = working_;
}

EditSession::Snapshot EditSession::make_snapshot(Profile profile) const {
    validate_profile(profile);
    const auto previous = working_ ? working_->revision() : 0;
    if (previous == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Edit revision exhausted");
    }
    return Snapshot(new EditSnapshot(identity_, std::move(profile), previous+1));
}

void EditSession::remember(std::deque<Snapshot>& history, const Snapshot& snapshot) {
    history.push_back(snapshot);
    if (history.size() > max_history) { history.pop_front(); }
}

bool EditSession::replace(Configuration configuration) {
    if (configuration == working().configuration) { return false; }
    auto next = make_snapshot({std::move(configuration), working().validation_sample_rate, working().channels});
    if (!editing_) { remember(undo_, working_); }
    else if (!recorded_edit_) { undo_.push_back(working_); }
    redo_.clear();
    working_ = std::move(next);
    if (editing_) { recorded_edit_ = true; }
    return true;
}

void EditSession::begin_edit() noexcept { editing_ = true; }

void EditSession::end_edit() noexcept {
    // A drag that returns to its starting value is not an undo step.
    if (recorded_edit_ && !undo_.empty() && undo_.back()->profile() == working()) { undo_.pop_back(); }
    if (undo_.size() > max_history) { undo_.pop_front(); }
    editing_ = recorded_edit_ = false;
}

bool EditSession::can_undo() const noexcept {
    if (recorded_edit_ && !undo_.empty() && undo_.back()->profile() == working()) { return undo_.size() > 1; }
    return !undo_.empty();
}

bool EditSession::restore(std::deque<Snapshot>& source, std::deque<Snapshot>& destination) {
    end_edit();
    if (source.empty()) { return false; }
    auto next = make_snapshot(source.back()->profile());
    remember(destination, working_);
    source.pop_back();
    working_ = std::move(next);
    return true;
}

bool EditSession::undo() { return restore(undo_, redo_); }
bool EditSession::redo() { return restore(redo_, undo_); }

void EditSession::save(const std::filesystem::path& path) {
    save_with([&](const Profile& profile) { save_profile_file(path, profile); });
}

void EditSession::save_with(const std::function<void(const Profile&)>& writer) {
    const auto snapshot = working_;
    writer(snapshot->profile());
    saved_ = snapshot;
    end_edit();
}

bool EditSession::revert() {
    const bool changed = dirty();
    auto next = changed ? make_snapshot(saved()) : working_;
    apply_revert(std::move(next));
    return changed;
}

void EditSession::apply_revert(Snapshot next) noexcept {
    undo_.clear(); redo_.clear();
    editing_ = recorded_edit_ = false;
    working_ = std::move(next);
}

bool EditSession::mark_applied(const Snapshot& snapshot) {
    if (!snapshot || snapshot->owner_ != identity_) {
        throw std::invalid_argument("Applied update does not belong to this edit session");
    }
    if (applied_ && snapshot->revision() <= applied_->revision()) { return false; }
    applied_ = snapshot;
    return true;
}

} // namespace contour::eq
