#include <contour/profile_library.h>
#include "file_storage.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace contour::eq {
namespace {
void require(bool condition,const char* message) { if (!condition) { throw std::invalid_argument(message); } }
bool valid_id(std::string_view id) {
    if (id.size()!=36 || id=="00000000-0000-0000-0000-000000000000") { return false; }
    for (std::size_t i=0;i<id.size();++i) {
        if (i==8 || i==13 || i==18 || i==23) { if (id[i]!='-') { return false; } }
        else if (!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f'))) { return false; }
    }
    return true;
}
std::string new_id() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) { throw std::runtime_error("Cannot generate profile identity"); }
    wchar_t text[40]{}; StringFromGUID2(guid,text,40);
    std::string id; id.reserve(36);
    for (unsigned i=1;i<37;++i) { const auto c=text[i]; id.push_back(static_cast<char>(c>=L'A' && c<=L'F' ? c+(L'a'-L'A') : c)); }
    return id;
}
void validate_name(std::string_view name) {
    require(!name.empty() && name.size()<=256,"Profile name must contain 1-256 UTF-8 bytes");
    bool nonspace=false;
    for (const unsigned char c : name) { require(c>=32 && c!=127,"Profile name contains a control character"); nonspace|=c!=' '; }
    require(nonspace,"Profile name must not be blank");
    require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0)>0,"Profile name is not valid UTF-8");
}
std::string encode_endpoint(std::wstring_view endpoint) {
    require(!endpoint.empty() && endpoint.size()<=1024,"Endpoint identity must contain 1-1024 UTF-8 bytes");
    for (const auto c : endpoint) { require(c>=32 && c!=127,"Endpoint identity contains a control character"); }
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,endpoint.data(),static_cast<int>(endpoint.size()),nullptr,0,nullptr,nullptr);
    require(size>0 && size<=1024,"Endpoint identity is invalid UTF-16 or too long");
    std::string result(static_cast<std::size_t>(size),'\0');
    require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,endpoint.data(),static_cast<int>(endpoint.size()),result.data(),size,nullptr,nullptr)==size,"Cannot encode endpoint identity");
    return result;
}
std::wstring decode_endpoint(std::string_view endpoint) {
    require(!endpoint.empty() && endpoint.size()<=1024,"Invalid endpoint identity size");
    const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,endpoint.data(),static_cast<int>(endpoint.size()),nullptr,0);
    require(size>0,"Endpoint identity is invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(size),L'\0');
    require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,endpoint.data(),static_cast<int>(endpoint.size()),result.data(),size)==size,"Cannot decode endpoint identity");
    (void)encode_endpoint(result); return result;
}
void set_assignment(std::vector<DeviceAssignment>& assignments,const DeviceAssignment& assignment) {
    const auto found=std::find_if(assignments.begin(),assignments.end(),[&](const auto& a) { return a.endpoint_id==assignment.endpoint_id; });
    if (found==assignments.end()) { assignments.push_back(assignment); } else { *found=assignment; }
}
std::uint64_t number(std::istringstream& row) {
    std::string token; row>>token; std::uint64_t result=0;
    const auto parsed=std::from_chars(token.data(),token.data()+token.size(),result);
    require(parsed.ec==std::errc{} && parsed.ptr==token.data()+token.size(),"Invalid library number"); return result;
}
std::string quoted(std::istringstream& row) {
    row>>std::ws; require(row.peek()=='"',"Expected quoted library field"); std::string text; row>>std::quoted(text); return text;
}
void end_row(std::istringstream& row) { require(!row.fail(),"Incomplete library field"); row>>std::ws; require(row.eof(),"Unexpected library fields"); }
std::istringstream row(std::string_view& text) {
    const auto end=text.find('\n'); require(end!=text.npos && end<=4096,"Invalid library metadata line");
    std::istringstream result{std::string(text.substr(0,end))}; result.imbue(std::locale::classic()); text.remove_prefix(end+1); return result;
}
struct LibraryData { std::vector<ProfileEntry> entries; std::vector<DeviceAssignment> assignments; };
LibraryData parse_library(std::string_view text) {
    require(text.size()<=ProfileLibrary::max_bytes,"Profile library exceeds 32 MiB");
    auto header=row(text); std::string tag,generation; header>>tag;
    const auto version=number(header);
    require(tag=="CONTOUR_LIBRARY" && (version==1 || version==2),"Unsupported profile library version");
    header>>generation; const auto count=number(header); const auto assignment_count=version==2 ? number(header) : 0; end_row(header);
    require(valid_id(generation),"Invalid profile library header");
    require(count<=ProfileLibrary::max_profiles,"Too many profiles in library");
    require(assignment_count<=ProfileLibrary::max_assignments,"Too many device assignments in library");
    std::vector<ProfileEntry> entries; entries.reserve(static_cast<std::size_t>(count)); std::set<std::string> ids;
    for (std::uint64_t i=0;i<count;++i) {
        auto metadata=row(text); metadata>>tag;
        ProfileEntry entry; entry.id=quoted(metadata); entry.revision=number(metadata); entry.name=quoted(metadata);
        const auto size=number(metadata); end_row(metadata);
        require(tag=="Entry" && valid_id(entry.id) && entry.revision>0 && ids.insert(entry.id).second,"Invalid or duplicate profile identity/revision");
        validate_name(entry.name);
        require(size>0 && size<=1024*1024 && size<=text.size(),"Invalid embedded profile size");
        entry.saved=parse_profile(text.substr(0,static_cast<std::size_t>(size))); text.remove_prefix(static_cast<std::size_t>(size));
        entries.push_back(std::move(entry));
    }
    std::vector<DeviceAssignment> assignments; assignments.reserve(static_cast<std::size_t>(assignment_count)); std::set<std::wstring> endpoints;
    for (std::uint64_t i=0;i<assignment_count;++i) {
        auto metadata=row(text); metadata>>tag;
        DeviceAssignment assignment; assignment.endpoint_id=decode_endpoint(quoted(metadata)); assignment.profile_id=quoted(metadata);
        assignment.revision=number(metadata); end_row(metadata);
        require(tag=="Assignment" && assignment.revision>0 && ids.contains(assignment.profile_id) && endpoints.insert(assignment.endpoint_id).second,
                "Invalid, duplicate, or dangling device assignment");
        assignments.push_back(std::move(assignment));
    }
    require(text=="EndLibrary\n","Incomplete library or trailing data"); return {std::move(entries),std::move(assignments)};
}
std::string serialize_library(const std::vector<ProfileEntry>& entries,const std::vector<DeviceAssignment>& assignments) {
    require(entries.size()<=ProfileLibrary::max_profiles,"Too many profiles in library");
    require(assignments.size()<=ProfileLibrary::max_assignments,"Too many device assignments in library");
    std::ostringstream out; out.imbue(std::locale::classic());
    out<<"CONTOUR_LIBRARY 2 "<<new_id()<<' '<<entries.size()<<' '<<assignments.size()<<'\n'; std::set<std::string> ids;
    for (const auto& entry : entries) {
        validate_name(entry.name); require(valid_id(entry.id) && entry.revision>0 && ids.insert(entry.id).second,"Invalid profile identity/revision");
        const auto content=serialize_profile(entry.saved);
        out<<"Entry "<<std::quoted(entry.id)<<' '<<entry.revision<<' '<<std::quoted(entry.name)<<' '<<content.size()<<'\n'<<content;
        require(out.tellp()>=0 && static_cast<std::uint64_t>(out.tellp())<=ProfileLibrary::max_bytes-11,"Profile library exceeds 32 MiB");
    }
    std::set<std::wstring> endpoints;
    for (const auto& assignment : assignments) {
        const auto endpoint=encode_endpoint(assignment.endpoint_id);
        require(assignment.revision>0 && ids.contains(assignment.profile_id) && endpoints.insert(assignment.endpoint_id).second,"Invalid device assignment");
        out<<"Assignment "<<std::quoted(endpoint)<<' '<<std::quoted(assignment.profile_id)<<' '<<assignment.revision<<'\n';
    }
    out<<"EndLibrary\n"; auto result=out.str(); require(result.size()<=ProfileLibrary::max_bytes,"Profile library exceeds 32 MiB"); return result;
}
struct LibraryLock {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit LibraryLock(const std::filesystem::path& path) {
        const auto lock_path=path.wstring()+L".lock";
        handle=CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (handle==INVALID_HANDLE_VALUE) { throw LibraryConflict("Cannot lock profile library; another writer or a storage error may be blocking it"); }
    }
    ~LibraryLock() { if (handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
};
std::uint64_t next_revision(std::uint64_t revision) {
    require(revision<std::numeric_limits<std::uint64_t>::max(),"Profile revision exhausted"); return revision+1;
}
}

ProfileLibrary::ProfileLibrary(std::filesystem::path path) : path_(std::filesystem::absolute(std::move(path)).lexically_normal()) { reload(); }
const ProfileEntry& ProfileLibrary::get(std::string_view id) const {
    const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e) { return e.id==id; });
    if (found==entries_.end()) { throw std::out_of_range("Profile is not in the library"); } return *found;
}
void ProfileLibrary::reload() {
    auto text=detail::read_file(path_,max_bytes);
    auto data=text ? parse_library(*text) : LibraryData{};
    entries_.swap(data.entries); assignments_.swap(data.assignments); baseline_=std::move(text);
}
void ProfileLibrary::commit(std::vector<ProfileEntry> entries) {
    commit(std::move(entries),assignments_);
}
void ProfileLibrary::commit(std::vector<ProfileEntry> entries,std::vector<DeviceAssignment> assignments) {
    auto text=serialize_library(entries,assignments);
    LibraryLock lock(path_);
    if (detail::read_file(path_,max_bytes)!=baseline_) { throw LibraryConflict("Profile library changed on disk; reload it before retrying"); }
    detail::write_file(path_,text);
    entries_.swap(entries); assignments_.swap(assignments); baseline_=std::move(text);
}
std::optional<DeviceAssignment> ProfileLibrary::assignment(std::wstring_view endpoint_id) const {
    (void)encode_endpoint(endpoint_id);
    const auto found=std::find_if(assignments_.begin(),assignments_.end(),[&](const auto& a) { return a.endpoint_id==endpoint_id; });
    return found==assignments_.end() ? std::nullopt : std::optional{*found};
}
void ProfileLibrary::check_assignment(std::wstring_view endpoint_id,const std::optional<DeviceAssignment>& expected) const {
    if (assignment(endpoint_id)!=expected) { throw LibraryConflict("Device assignment changed; reopen its profile session before applying edits"); }
}
DeviceAssignment ProfileLibrary::prepare_assignment(std::wstring_view endpoint_id,const std::optional<DeviceAssignment>& expected,std::string profile_id) const {
    check_assignment(endpoint_id,expected);
    require(expected || assignments_.size()<max_assignments,"Device assignment library is full");
    return {std::wstring(endpoint_id),std::move(profile_id),expected ? next_revision(expected->revision) : 1};
}
ProfileEntry ProfileLibrary::prepare_create(std::string name,Profile saved) const {
    validate_name(name); validate_profile(saved); require(entries_.size()<max_profiles,"Profile library is full");
    return {new_id(),std::move(name),1,std::move(saved)};
}
void ProfileLibrary::insert(const ProfileEntry& entry,const std::optional<DeviceAssignment>& assignment) {
    require(std::none_of(entries_.begin(),entries_.end(),[&](const auto& existing) { return existing.id==entry.id; }),"Profile identity already exists");
    auto entries=entries_; entries.push_back(entry); auto assignments=assignments_;
    if (assignment) { set_assignment(assignments,*assignment); }
    commit(std::move(entries),std::move(assignments));
}
ProfileEntry ProfileLibrary::create(std::string name,Profile saved) {
    auto entry=prepare_create(std::move(name),std::move(saved)); insert(entry); return entry;
}
ProfileEntry ProfileLibrary::duplicate_saved(std::string_view id,std::string name) { return create(std::move(name),get(id).saved); }
ProfileEntry ProfileLibrary::update(std::string_view id,std::uint64_t expected_revision,const Profile& saved) {
    auto entry=get(id); if (entry.revision!=expected_revision) { throw LibraryConflict("Profile changed since this draft was opened"); }
    validate_profile(saved); entry.saved=saved; entry.revision=next_revision(entry.revision);
    auto entries=entries_; for (auto& candidate : entries) { if (candidate.id==id) { candidate=entry; break; } }
    commit(std::move(entries)); return entry;
}
ProfileEntry ProfileLibrary::rename(std::string_view id,std::uint64_t expected_revision,std::string name) {
    auto entry=get(id); if (entry.revision!=expected_revision) { throw LibraryConflict("Profile changed since this draft was opened"); }
    validate_name(name); entry.name=std::move(name); entry.revision=next_revision(entry.revision);
    auto entries=entries_; for (auto& candidate : entries) { if (candidate.id==id) { candidate=entry; break; } }
    commit(std::move(entries)); return entry;
}

ProfileDocument::ProfileDocument(ProfileEntry entry) : entry_(std::move(entry)),editor_(std::make_shared<EditSession>(entry_.saved)) {}
ProfileWorkspace::ProfileWorkspace(ProfileLibrary& library,std::wstring endpoint_id)
    : library_(library),endpoint_id_(std::move(endpoint_id)),assignment_(library.assignment(*endpoint_id_)) {
    if (assignment_) {
        current_=std::shared_ptr<ProfileDocument>(new ProfileDocument(library_.get(assignment_->profile_id)));
        documents_.emplace(current_->id(),current_);
    }
}
void ProfileWorkspace::verify_assignment() const {
    if (!endpoint_id_) { return; }
    library_.check_assignment(*endpoint_id_,assignment_);
    if (current_ && library_.get(current_->id()).revision!=current_->entry_.revision) {
        throw LibraryConflict("Selected profile changed; reopen its session before applying device settings");
    }
    if (detail::read_file(library_.path_,ProfileLibrary::max_bytes)!=library_.baseline_) {
        throw LibraryConflict("Profile library changed on disk; reload it before applying device settings");
    }
}
ProfileDocument& ProfileWorkspace::selected() const {
    if (!current_) { throw std::logic_error("No profile is selected"); } return *current_;
}
std::optional<SelectionResult> ProfileWorkspace::guard(std::optional<UnsavedChoice> choice) const {
    if (choice) {
        require(*choice==UnsavedChoice::save || *choice==UnsavedChoice::discard || *choice==UnsavedChoice::cancel,"Invalid unsaved-changes choice");
        if (*choice==UnsavedChoice::cancel) { return SelectionResult::cancelled; }
    }
    if (current_ && current_->editor_->dirty() && !choice) { return SelectionResult::needs_decision; }
    return {};
}
void ProfileWorkspace::finish_current(std::optional<UnsavedChoice> choice) {
    if (!current_) { return; }
    if (current_->editor_->dirty()) {
        if (choice==UnsavedChoice::save) { save(); }
        else if (choice==UnsavedChoice::discard) { current_->editor_->revert(); }
        else { throw std::logic_error("Unsaved changes require a decision"); }
    }
    current_->editor_->clear_applied();
}
SelectionResult ProfileWorkspace::select(std::string_view id,std::optional<UnsavedChoice> choice) {
    if (current_ && current_->id()==id) { return SelectionResult::unchanged; }
    const auto entry=library_.get(id); // Check the target before resolving source edits.
    if (const auto blocked=guard(choice)) { return *blocked; }
    auto found=documents_.find(id);
    std::shared_ptr<ProfileDocument> next;
    if (found!=documents_.end() && found->second->entry_.revision==entry.revision) { next=found->second; }
    else {
        if (found!=documents_.end() && found->second->editor_->dirty()) { throw LibraryConflict("An inactive draft conflicts with a changed library profile"); }
        next=std::shared_ptr<ProfileDocument>(new ProfileDocument(entry));
        documents_.insert_or_assign(entry.id,next);
    }
    if (endpoint_id_) {
        auto assignment=library_.prepare_assignment(*endpoint_id_,assignment_,entry.id);
        auto entries=library_.entries_; auto assignments=library_.assignments_; set_assignment(assignments,assignment);
        std::optional<ProfileEntry> updated; EditSession::Snapshot discarded;
        if (current_ && current_->editor_->dirty()) {
            if (choice==UnsavedChoice::save) {
                updated=current_->entry_;
                if (library_.get(updated->id).revision!=updated->revision) { throw LibraryConflict("Profile changed since this draft was opened"); }
                updated->saved=current_->editor_->working(); updated->revision=next_revision(updated->revision);
                for (auto& candidate : entries) { if (candidate.id==updated->id) { candidate=*updated; break; } }
            } else {
                discarded=current_->editor_->make_snapshot(current_->editor_->saved());
            }
        }
        // All allocation/validation precedes the commit. Checkpoint, discard,
        // assignment, and selected-editor changes after it cannot fail.
        if (updated) {
            current_->editor_->save_with([&](const Profile&) { library_.commit(std::move(entries),std::move(assignments)); });
            current_->entry_=std::move(*updated);
        } else { library_.commit(std::move(entries),std::move(assignments)); }
        if (discarded) { current_->editor_->apply_revert(std::move(discarded)); }
        if (current_) { current_->editor_->clear_applied(); }
        assignment_=std::move(assignment);
    } else { finish_current(choice); }
    next->editor_->clear_applied(); current_=std::move(next); return SelectionResult::selected;
}
SelectionResult ProfileWorkspace::close(std::optional<UnsavedChoice> choice) {
    if (!current_) { return SelectionResult::unchanged; }
    if (const auto blocked=guard(choice)) { return *blocked; }
    finish_current(choice); current_.reset(); return SelectionResult::closed;
}
void ProfileWorkspace::save() {
    if (endpoint_id_) { library_.check_assignment(*endpoint_id_,assignment_); }
    auto& doc=selected(); ProfileEntry updated;
    doc.editor_->save_with([&](const Profile& profile) { updated=library_.update(doc.id(),doc.entry_.revision,profile); });
    doc.entry_=std::move(updated);
}
bool ProfileWorkspace::revert() { return selected().editor_->revert(); }
void ProfileWorkspace::rename(std::string name) {
    auto& doc=selected(); auto renamed=library_.rename(doc.id(),doc.entry_.revision,std::move(name)); doc.entry_=std::move(renamed);
}
ProfileEntry ProfileWorkspace::duplicate(std::string name) { return library_.create(std::move(name),selected().editor_->working()); }
ProfileEntry ProfileWorkspace::save_as(std::string name) {
    auto& source=selected(); auto entry=library_.prepare_create(std::move(name),source.editor_->working());
    auto assignment=endpoint_id_ ? std::optional{library_.prepare_assignment(*endpoint_id_,assignment_,entry.id)} : std::nullopt;
    auto next=std::shared_ptr<ProfileDocument>(new ProfileDocument(entry));
    auto restored=std::make_shared<EditSession>(source.editor_->saved());
    const auto [position,inserted]=documents_.emplace(entry.id,next); require(inserted,"Profile identity already open");
    try { library_.insert(entry,assignment); } catch (...) { documents_.erase(position); throw; }
    // All allocations are staged before persistence; the remaining selection
    // and source reset cannot fail after the new profile is committed.
    source.editor_->clear_applied(); source.editor_=std::move(restored); current_=std::move(next);
    assignment_=std::move(assignment);
    return entry;
}
} // namespace contour::eq
