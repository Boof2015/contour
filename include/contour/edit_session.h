#pragma once

#include <contour/profile.h>
#include <deque>
#include <functional>
#include <memory>

namespace contour::eq {

// An immutable, validated update. A publisher retains this handle until the
// audio runtime confirms its revision, even if the user has made newer edits.
class EditSnapshot final {
public:
    EditSnapshot(const EditSnapshot&) = delete;
    EditSnapshot& operator=(const EditSnapshot&) = delete;
    const Profile& profile() const noexcept { return profile_; }
    std::uint64_t revision() const noexcept { return revision_; }
private:
    friend class EditSession;
    struct Identity {};
    EditSnapshot(std::shared_ptr<const Identity> owner, Profile profile, std::uint64_t revision);
    std::shared_ptr<const Identity> owner_;
    Profile profile_;
    std::uint64_t revision_;
};

// One profile at a fixed validation rate/channel count. All operations belong
// on one application/control thread, never in an audio callback. Device I/O,
// publication, and UI gestures live outside this model.
class EditSession final {
public:
    using Snapshot = std::shared_ptr<const EditSnapshot>;
    static constexpr std::size_t max_history = 200;

    explicit EditSession(Profile saved);
    EditSession(const EditSession&) = delete;
    EditSession& operator=(const EditSession&) = delete;

    const Profile& saved() const noexcept { return saved_->profile(); }
    const Profile& working() const noexcept { return working_->profile(); }
    bool dirty() const noexcept { return working() != saved(); }
    Snapshot snapshot() const noexcept { return working_; }
    Snapshot applied() const noexcept { return applied_; }
    bool awaiting_preview() const noexcept { return applied_ != working_; }

    // Replace is an atomic edit: invalid settings leave all state unchanged.
    // Imported layers and manual edits go through this same path. No-op edits
    // create no history/revision. Format changes require a new session.
    bool replace(Configuration configuration);

    // Optional grouping for a drag/scrub. Each valid value can be auditioned,
    // while the whole gesture is one undo step. Begin/end are idempotent.
    void begin_edit() noexcept;
    void end_edit() noexcept;
    bool can_undo() const noexcept;
    bool can_redo() const noexcept { return !redo_.empty(); }
    bool undo();
    bool redo();

    // Save changes the checkpoint only after an atomic file write succeeds.
    // It retains history. Revert restores the checkpoint and clears history.
    // Neither operation asserts that audio has applied anything.
    void save(const std::filesystem::path& path);
    // Synchronous persistence adapter. The writer must persist the complete
    // profile or throw; a failure leaves the checkpoint and history unchanged.
    void save_with(const std::function<void(const Profile&)>& writer);
    bool revert();

    // Call only after actual audio-runtime acknowledgement, not on enqueue or
    // file-write success. Late acknowledgements cannot move applied backwards;
    // handles from another session (including a reopened profile) are rejected.
    bool mark_applied(const Snapshot& snapshot);
    // A stopped/replaced audio runtime invalidates its last confirmation.
    void clear_applied() noexcept { applied_.reset(); }

private:
    friend class ProfileWorkspace;
    Snapshot make_snapshot(Profile profile) const;
    void apply_revert(Snapshot next) noexcept;
    static void remember(std::deque<Snapshot>& history, const Snapshot& snapshot);
    bool restore(std::deque<Snapshot>& source, std::deque<Snapshot>& destination);

    std::shared_ptr<const EditSnapshot::Identity> identity_;
    Snapshot saved_, working_, applied_;
    std::deque<Snapshot> undo_, redo_;
    bool editing_ = false, recorded_edit_ = false;
};

} // namespace contour::eq
