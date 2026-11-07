#pragma once
#include <contour/edit_session.h>
#include <map>
#include <stdexcept>

namespace contour::eq {

struct ProfileEntry {
    std::string id; // Stable identity; names are display text, never paths.
    std::string name;
    std::uint64_t revision=1;
    Profile saved;
    bool operator==(const ProfileEntry&) const = default;
};

struct DeviceAssignment {
    std::wstring endpoint_id; // Exact opaque Windows identity, never a name/path.
    std::string profile_id;
    std::uint64_t revision=1;
    bool operator==(const DeviceAssignment&) const = default;
};

class LibraryConflict : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// App/control-thread storage for profiles and remembered device selections. The supplied
// path's parent must already exist. Opening a missing library is read-only;
// the first mutation creates it. Invalid existing data is never overwritten.
// Cooperative writers use an adjacent lock plus compare-before-replace. A
// stale library must be explicitly reloaded; per-profile revisions also guard
// drafts opened before that reload. Limits bound parsing and memory use.
class ProfileLibrary final {
public:
    static constexpr std::size_t max_profiles=128;
    static constexpr std::size_t max_assignments=128;
    static constexpr std::size_t max_bytes=32*1024*1024;
    explicit ProfileLibrary(std::filesystem::path path);
    const std::vector<ProfileEntry>& entries() const noexcept { return entries_; }
    const std::vector<DeviceAssignment>& assignments() const noexcept { return assignments_; }
    std::optional<DeviceAssignment> assignment(std::wstring_view endpoint_id) const;
    const ProfileEntry& get(std::string_view id) const;
    void reload();
    ProfileEntry create(std::string name,Profile saved);
    ProfileEntry duplicate_saved(std::string_view id,std::string name);
    ProfileEntry update(std::string_view id,std::uint64_t expected_revision,const Profile& saved);
    ProfileEntry rename(std::string_view id,std::uint64_t expected_revision,std::string name);
    // Deletion never unassigns a device or changes its saved/running EQ.
    // Assigned profiles (including disconnected devices) must be replaced first.
    void erase(std::string_view id,std::uint64_t expected_revision);
private:
    friend class ProfileWorkspace;
    ProfileEntry prepare_create(std::string name,Profile saved) const;
    DeviceAssignment prepare_assignment(std::wstring_view endpoint_id,const std::optional<DeviceAssignment>& expected,std::string profile_id) const;
    void check_assignment(std::wstring_view endpoint_id,const std::optional<DeviceAssignment>& expected) const;
    void insert(const ProfileEntry& entry,const std::optional<DeviceAssignment>& assignment = {});
    void commit(std::vector<ProfileEntry> entries);
    void commit(std::vector<ProfileEntry> entries,std::vector<DeviceAssignment> assignments);
    std::filesystem::path path_;
    std::optional<std::string> baseline_;
    std::vector<ProfileEntry> entries_;
    std::vector<DeviceAssignment> assignments_;
};

enum class UnsavedChoice { save, discard, cancel };
enum class SelectionResult { selected, unchanged, needs_decision, cancelled, closed };

class ProfileDocument final {
public:
    const std::string& id() const noexcept { return entry_.id; }
    const std::string& name() const noexcept { return entry_.name; }
    std::shared_ptr<EditSession> editor() const noexcept { return editor_; }
private:
    friend class ProfileWorkspace;
    explicit ProfileDocument(ProfileEntry entry);
    ProfileEntry entry_;
    std::shared_ptr<EditSession> editor_;
};

// One editor selection and per-profile undo histories. No implicit device
// writes, preview, sharing, or channel mapping. Retained document/editor handles
// remain alive across a switch, but bindings must follow current() and detach
// from the old editor. Selection changes clear old audio confirmations.
class ProfileWorkspace final {
public:
    explicit ProfileWorkspace(ProfileLibrary& library) : library_(library) {}
    // Restores a remembered selection without writes or preview. Bound switches
    // and Save as persist their assignment in the same library transaction.
    ProfileWorkspace(ProfileLibrary& library,std::wstring endpoint_id);
    std::shared_ptr<const ProfileDocument> current() const noexcept { return current_; }
    SelectionResult select(std::string_view id,std::optional<UnsavedChoice> choice = {});
    SelectionResult close(std::optional<UnsavedChoice> choice = {});
    void save();
    bool revert();
    void rename(std::string name);
    // Duplicate current edits without changing the source or selection.
    ProfileEntry duplicate(std::string name);
    // Save current edits as a new identity, select it, and restore the old
    // profile to its checkpoint with cleared history, as in the design.
    // This replaces the source document's editor; retained old editor handles
    // remain valid but detached. Rebind previews to current()->editor().
    ProfileEntry save_as(std::string name);
    // Before explicit device I/O: reject a stale assignment/catalog rather
    // than overwrite a newer choice. Read-only; does not replace any draft.
    void verify_assignment() const;
private:
    ProfileDocument& selected() const;
    std::optional<SelectionResult> guard(std::optional<UnsavedChoice> choice) const;
    void finish_current(std::optional<UnsavedChoice> choice);
    ProfileLibrary& library_;
    std::shared_ptr<ProfileDocument> current_;
    std::map<std::string,std::shared_ptr<ProfileDocument>,std::less<>> documents_;
    std::optional<std::wstring> endpoint_id_;
    std::optional<DeviceAssignment> assignment_;
};
} // namespace contour::eq
