#include <contour/profile_library.h>
#include <contour/live_control.h>
#include "control_test_support.h"
#include "test_support.h"
#include <array>
#include <fstream>
#include <iterator>

using namespace contour::eq;
using test::require;
namespace {
Configuration gain(double value) { return {{{"All",{},{},value}}}; }
Profile profile(double value) { return {gain(value),48000,2}; }
template<class F> void fails(F action) {
    bool failed=false; try { action(); } catch (const std::exception&) { failed=true; }
    require(failed,"Expected rejected operation");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); require(file.good(),"Read library fixture");
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path,const std::string& text) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc); file<<text; file.close(); require(!file.fail(),"Write library fixture");
}
struct FileLock {
    HANDLE handle;
    explicit FileLock(const std::filesystem::path& path) : handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)) {
        require(handle!=INVALID_HANDLE_VALUE,"Lock file fixture");
    }
    ~FileLock() { CloseHandle(handle); }
};
void storage(const std::filesystem::path& directory) {
    const auto path=directory/L"storage.contour-library";
    ProfileLibrary library(path); require(library.entries().empty() && !std::filesystem::exists(path),"Opening missing library wrote data");
    const auto empty=library.create("Flat",{{},96000,4});
    const auto room=library.create("部屋 / Studio",profile(-8));
    const auto copy=library.duplicate_saved(room.id,"Room copy");
    require(copy.id!=room.id && copy.saved==room.saved && copy.revision==1,"Duplicate aliased original identity or data");
    const auto renamed=library.rename(room.id,room.revision,"../Display name, not a path");
    require(renamed.id==room.id && renamed.saved==room.saved && renamed.revision>room.revision,"Rename changed identity or EQ");
    ProfileLibrary reopened(path); require(reopened.entries()==library.entries(),"Reopen lost library metadata/profile data");
    require(reopened.get(empty.id).saved.channels==4 && reopened.get(empty.id).saved.configuration.layers.empty(),"New empty profile invented EQ or changed format");
    const auto before=read(path);
    for (const auto& name : {std::string{},std::string("   "),std::string("x\ny"),std::string("x\0y",3),std::string(257,'a'),std::string("\xff")}) {
        fails([&] { library.create(name,profile(-1)); });
    }
    fails([&] { library.create("Invalid EQ",profile(80)); });
    require(read(path)==before && library.entries().size()==3,"Invalid create modified library");
}
void switching(const std::filesystem::path& directory) {
    const auto path=directory/L"switching.contour-library";
    ProfileLibrary library(path); const auto a=library.create("Studio",profile(-2)), b=library.create("Late night",profile(-10));
    ProfileWorkspace workspace(library); require(workspace.select(a.id)==SelectionResult::selected,"Initial selection failed");
    auto first=workspace.current()->editor(); first->replace(gain(-6)); first->mark_applied(first->snapshot());
    const auto snapshot=first->snapshot(); const auto disk=read(path);
    require(workspace.select(b.id)==SelectionResult::needs_decision,"Dirty switch skipped Save/Don't save/Cancel");
    require(workspace.select(b.id,UnsavedChoice::cancel)==SelectionResult::cancelled,"Cancel not reported");
    require(workspace.select(a.id,UnsavedChoice::discard)==SelectionResult::unchanged,"Reselecting active profile discarded edits");
    fails([&] { workspace.select("missing",UnsavedChoice::save); });
    require(first->snapshot()==snapshot && first->applied()==snapshot && workspace.current()->id()==a.id && read(path)==disk,"Prompt/cancel/invalid target changed selection, edits, audio claim, or disk");
    require(workspace.select(b.id,UnsavedChoice::save)==SelectionResult::selected,"Save and switch failed");
    require(!first->dirty() && first->can_undo() && !first->applied() && library.get(a.id).saved==profile(-6),"Save switch lost history or retained audio claim");
    require(!workspace.current()->editor()->applied(),"New profile inherited old audio confirmation");
    workspace.select(a.id); require(workspace.current()->editor()==first && first->undo() && first->working()==profile(-2),"Per-profile undo history did not survive switching");
    workspace.select(b.id,UnsavedChoice::discard);
    require(first->working()==profile(-6) && !first->can_undo() && !first->can_redo(),"Don't save did not restore checkpoint and clear history");
    workspace.current()->editor()->replace(gain(-15));
    require(workspace.close()==SelectionResult::needs_decision && workspace.close(UnsavedChoice::cancel)==SelectionResult::cancelled,"Close lost dirty-profile guard");
    require(workspace.close(UnsavedChoice::save)==SelectionResult::closed && !workspace.current(),"Save and close failed");
    ProfileLibrary reopened(path); require(reopened.get(b.id).saved==profile(-15),"Close save did not persist");
}
void copies(const std::filesystem::path& directory) {
    ProfileLibrary library(directory/L"copies.contour-library"); const auto original=library.create("Studio",profile(-3));
    ProfileWorkspace workspace(library); workspace.select(original.id);
    const auto source=workspace.current(); auto editor=source->editor(); editor->replace(gain(-8)); editor->mark_applied(editor->snapshot());
    const auto snapshot=editor->snapshot(); const auto duplicate=workspace.duplicate("Studio copy");
    require(duplicate.saved==profile(-8) && duplicate.id!=original.id && workspace.current()==source,"Duplicate selected itself or copied the wrong state");
    require(editor->snapshot()==snapshot && editor->dirty() && editor->applied()==snapshot && library.get(original.id).saved==profile(-3),"Duplicate altered the source draft/checkpoint");
    workspace.rename("Renamed studio");
    require(source->name()=="Renamed studio" && source->id()==original.id && editor->snapshot()==snapshot && editor->dirty(),"Rename changed editable EQ state");
    const auto saved_as=workspace.save_as("Night");
    require(workspace.current()->id()==saved_as.id && saved_as.id!=original.id && saved_as.saved==profile(-8),"Save as failed to select a new complete copy");
    require(!workspace.current()->editor()->dirty() && !workspace.current()->editor()->can_undo() && !workspace.current()->editor()->applied(),"Save as inherited dirty/history/applied state");
    require(source->editor()->working()==profile(-3) && !source->editor()->can_undo() && !editor->applied(),"Save as failed to restore original checkpoint or clear old binding's confirmation");
    workspace.current()->editor()->replace(gain(-12)); workspace.save();
    require(library.get(duplicate.id).saved==profile(-8) && library.get(original.id).saved==profile(-3),"Editing copy mutated another profile");
    workspace.current()->editor()->replace(gain(-1)); require(workspace.revert() && workspace.current()->editor()->working()==profile(-12),"Revert ignored the new saved checkpoint");
    ProfileLibrary reopened(directory/L"copies.contour-library"); require(reopened.entries()==library.entries(),"Copy/save-as identities were not durable");
}
void failed_writes(const std::filesystem::path& directory) {
    const auto path=directory/L"failure.contour-library";
    ProfileLibrary library(path); const auto a=library.create("A",profile(-2)), b=library.create("B",profile(-10));
    ProfileWorkspace workspace(library); workspace.select(a.id); auto editor=workspace.current()->editor(); editor->replace(gain(-8));
    const auto snapshot=editor->snapshot(); const auto bytes=read(path); const auto entries=library.entries();
    {
        FileLock lock(path); // Real Windows failure during atomic replacement.
        fails([&] { workspace.save(); });
        fails([&] { workspace.select(b.id,UnsavedChoice::save); });
        fails([&] { workspace.close(UnsavedChoice::save); });
        fails([&] { workspace.duplicate("Copy"); });
        fails([&] { workspace.save_as("Save as"); });
        fails([&] { workspace.rename("Changed"); });
        require(workspace.current()->id()==a.id && editor->snapshot()==snapshot && editor->dirty() && editor->can_undo(),"Failed persistence changed active editor state");
        require(library.entries()==entries && read(path)==bytes,"Failed persistence damaged library");
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        require(!entry.path().filename().wstring().starts_with(L"failure.contour-library.") || entry.path().extension()==L".lock","Failed save left a transaction file");
    }
    workspace.save(); require(!editor->dirty() && library.get(a.id).saved==profile(-8),"Retry after write failure did not recover");
    {
        FileLock lock(std::filesystem::path(path.wstring()+L".lock"));
        fails([&] { library.create("Busy",profile(0)); });
    }
    require(library.entries().size()==2,"Busy library accepted another writer");
}
void conflicts(const std::filesystem::path& directory) {
    const auto path=directory/L"conflicts.contour-library";
    ProfileLibrary first(path); const auto a=first.create("A",profile(-2));
    ProfileWorkspace workspace(first); workspace.select(a.id); workspace.current()->editor()->replace(gain(-7));
    ProfileLibrary second(path); second.create("Other writer",profile(-4));
    const auto latest=read(path);
    fails([&] { workspace.save(); });
    require(read(path)==latest && workspace.current()->editor()->dirty(),"Stale library overwrote another writer");
    first.reload(); workspace.save(); require(first.get(a.id).saved==profile(-7),"Unrelated-profile conflict did not recover after reload");
    second.reload(); second.update(a.id,second.get(a.id).revision,profile(-11)); first.reload();
    workspace.current()->editor()->replace(gain(-9)); fails([&] { workspace.save(); });
    require(first.get(a.id).saved==profile(-11) && workspace.current()->editor()->dirty(),"Reload allowed a stale draft to overwrite newer saved EQ");
    const auto rescue=workspace.save_as("Recovered draft");
    require(rescue.saved==profile(-9) && first.get(a.id).saved==profile(-11),"Save as failed to preserve both conflicting versions");
    workspace.select(a.id); require(workspace.current()->editor()->working()==profile(-11),"Returning to externally changed profile loaded a stale clean cache");
}
void corruption(const std::filesystem::path& directory) {
    const auto path=directory/L"corrupt.contour-library";
    ProfileLibrary library(path); library.create("Valid",profile(-4)); const auto good=read(path); const auto original=library.entries();
    std::vector<std::string> malformed{std::string{},good.substr(0,good.size()-1),good+"unexpected",good.substr(0,good.find("CONTOUR 1"))};
    auto bad=good; bad.replace(0,std::string("CONTOUR_LIBRARY 2").size(),"CONTOUR_LIBRARY 99"); malformed.push_back(bad);
    bad=good; bad.replace(bad.find("EndLayer"),8,"Invalid!"); malformed.push_back(bad);
    bad=good; const auto size_at=bad.find("\nEntry"); bad.replace(size_at-1,1,"999999"); malformed.push_back(bad);
    const auto entry_at=good.find("Entry ");
    const auto revision_at=entry_at+std::string("Entry \"").size()+36+std::string("\" ").size();
    bad=good; bad.replace(revision_at,1,"0"); malformed.push_back(bad);
    bad=good; bad.replace(revision_at,1,"18446744073709551616"); malformed.push_back(bad);
    bad=good; bad.replace(entry_at+7,1,"Z"); malformed.push_back(bad);
    // An otherwise valid second record may not reuse the first record's ID.
    bad=good; bad.replace(size_at-3,1,"2");
    bad.insert(bad.find("EndLibrary"),good.substr(entry_at,good.find("EndLibrary")-entry_at)); malformed.push_back(bad);
    for (const auto& bytes : malformed) {
        write(path,bytes); fails([&] { ProfileLibrary broken(path); }); fails([&] { library.reload(); });
        fails([&] { library.create("Do not overwrite",profile(0)); });
        require(read(path)==bytes && library.entries()==original,"Corrupt library was replaced or partially loaded");
    }
    write(path,good); library.reload(); require(library.entries()==original,"Restored library could not be reopened");
}
void preview_binding(const std::filesystem::path& directory) {
    ProfileLibrary library(directory/L"preview.contour-library"); const auto a=library.create("A",profile(-3)), b=library.create("B",profile(-12));
    ProfileWorkspace workspace(library); workspace.select(a.id); const auto old_editor=workspace.current()->editor();
    const auto store=Microsoft::WRL::Make<control_test::Store>();
    SmoothEq runtime; runtime.initialize(ResidentBank::prepare(gain(0),48000,2,1),960); PreviewProcessor processor(runtime,48000,2);
    const auto report=[&] { write_control_value(*store.Get(),preview_status_key,encode_status(processor.poll())); }; report();
    auto client=std::make_unique<LivePreview>(*old_editor,store.Get()); client->connect(); client->submit();
    const auto apply=[&](double db) {
        processor.submit(*read_control_value(*store.Get(),preview_request_key,max_request_bytes));
        std::array<float,4000> samples{}; samples.fill(0.1F); runtime.process(samples.data(),samples.data(),2000); report();
        require(client->poll().feedback==PreviewFeedback::applied,"Selected profile was not acknowledged");
        test::expect_near(samples.back(),0.1*std::pow(10.0,db/20),"Selected profile DSP output");
    }; apply(-3);
    workspace.select(b.id); require(!old_editor->applied() && !workspace.current()->editor()->applied(),"Profile switch transferred audio confirmation");
    client.reset(); const auto next_editor=workspace.current()->editor(); client=std::make_unique<LivePreview>(*next_editor,store.Get()); client->connect();
    require(!next_editor->applied(),"Binding a selected profile claimed it was audible");
    client->submit(); require(client->poll().feedback==PreviewFeedback::waiting,"Prior profile acknowledgement matched new editor"); apply(-12);
    const auto request=read_control_value(*store.Get(),preview_request_key,max_request_bytes);
    workspace.rename("B renamed"); workspace.duplicate("B copy");
    require(read_control_value(*store.Get(),preview_request_key,max_request_bytes)==request,"Library metadata/copy caused unsolicited audio changes");
}
void remembered_devices(const std::filesystem::path& directory) {
    const auto path=directory/L"assignments.library";
    ProfileLibrary library(path); const auto a=library.create("Same name",profile(-3)),b=library.create("Same name",profile(-12));
    const std::wstring endpoint=L"opaque \\ \" output / 部屋";
    ProfileWorkspace first(library,endpoint); require(!first.current() && library.assignments().empty(),"Unassigned device invented a selection");
    first.select(a.id); const auto initial=library.assignment(endpoint); require(initial && initial->profile_id==a.id && initial->revision==1,"Selection did not persist exact identity");
    first.current()->editor()->replace(gain(-7)); first.current()->editor()->mark_applied(first.current()->editor()->snapshot());
    const auto disk=read(path);
    require(first.select(b.id)==SelectionResult::needs_decision && first.select(b.id,UnsavedChoice::cancel)==SelectionResult::cancelled,"Bound selection lost dirty guard");
    require(read(path)==disk && library.assignment(endpoint)==initial,"Cancelled selection changed assignment");
    first.select(b.id,UnsavedChoice::save);
    require(library.get(a.id).saved==profile(-7) && library.assignment(endpoint)->profile_id==b.id,"Save-and-switch did not commit source and assignment together");
    ProfileWorkspace other(library,L"different opaque output"); other.select(a.id);
    require(library.assignment(endpoint)->profile_id==b.id && library.assignment(L"different opaque output")->profile_id==a.id,"Second device overwrote first assignment");
    first.rename("Renamed"); const auto assignment=library.assignment(endpoint); const auto duplicate=first.duplicate("Copy");
    require(library.assignment(endpoint)==assignment && duplicate.id!=assignment->profile_id,"Rename/duplicate changed assigned identity");
    first.current()->editor()->replace(gain(-9)); const auto saved_as=first.save_as("Saved as");
    require(library.assignment(endpoint)->profile_id==saved_as.id && library.get(b.id).saved==profile(-12),"Save as failed to reassign or changed the original");
    first.current()->editor()->replace(gain(-1));
    const auto before=read(path); ProfileLibrary reopened(path); ProfileWorkspace restored(reopened,endpoint);
    require(restored.current()->id()==saved_as.id && restored.current()->editor()->working()==profile(-9) && !restored.current()->editor()->dirty() && !restored.current()->editor()->can_undo() && !restored.current()->editor()->applied(),"Reopen restored draft/history/audio claims instead of the saved profile");
    require(read(path)==before && first.current()->editor()->dirty(),"Restore wrote the library or changed another draft");
    ProfileWorkspace unknown(reopened,L"Different opaque output"); require(!unknown.current(),"Opaque endpoint was normalized or matched by a name");
    first.close(UnsavedChoice::discard); require(library.assignment(endpoint)->profile_id==saved_as.id,"Closing an editor forgot its device assignment");
}
void assignment_failures(const std::filesystem::path& directory) {
    const auto path=directory/L"assignment-failure.library";
    ProfileLibrary library(path); const auto a=library.create("A",profile(-3)),b=library.create("B",profile(-12));
    ProfileWorkspace workspace(library,L"output"); workspace.select(a.id); const auto editor=workspace.current()->editor(); editor->replace(gain(-8)); editor->mark_applied(editor->snapshot());
    const auto snapshot=editor->snapshot(),applied=editor->applied(); const auto disk=read(path); const auto assignment=library.assignment(L"output");
    {
        FileLock lock(path);
        fails([&] { workspace.select(b.id,UnsavedChoice::save); });
        fails([&] { workspace.select(b.id,UnsavedChoice::discard); });
        fails([&] { workspace.save_as("New"); });
    }
    require(workspace.current()->id()==a.id && editor->snapshot()==snapshot && editor->applied()==applied && editor->dirty() && editor->can_undo(),"Failed assignment commit lost draft or selection");
    require(read(path)==disk && library.assignment(L"output")==assignment && library.entries().size()==2 && library.get(a.id).saved==profile(-3),"Failed assignment partially saved a profile or binding");
    workspace.select(b.id,UnsavedChoice::discard);
    require(editor->working()==profile(-3) && !editor->can_undo() && library.assignment(L"output")->profile_id==b.id,"Retry failed to discard and select atomically");
}
void assignment_conflicts(const std::filesystem::path& directory) {
    const auto path=directory/L"assignment-conflict.library";
    ProfileLibrary library(path); const auto a=library.create("A",profile(-3)),b=library.create("B",profile(-12));
    ProfileWorkspace stale(library,L"output"),first(library,L"output"); first.select(a.id);
    fails([&] { stale.select(b.id); }); require(!stale.current(),"Stale unassigned workspace overwrote first assignment");
    ProfileWorkspace competitor(library,L"output"); competitor.select(b.id); first.current()->editor()->replace(gain(-8));
    fails([&] { first.verify_assignment(); }); fails([&] { first.save(); }); fails([&] { first.save_as("Stale copy"); });
    require(library.assignment(L"output")->profile_id==b.id && first.current()->editor()->dirty() && library.entries().size()==2,"Stale workspace overwrote newer selection or lost its draft");
    ProfileLibrary external(path); ProfileWorkspace external_session(external,L"output"); external_session.select(a.id);
    fails([&] { competitor.verify_assignment(); }); // No reload: on-disk comparison catches the other writer.
    library.reload(); fails([&] { competitor.verify_assignment(); }); fails([&] { competitor.select(a.id); });
    ProfileWorkspace fresh(library,L"output"); require(fresh.current()->id()==a.id,"Reopen did not recover newer assignment");
    external.reload(); external.update(a.id,external.get(a.id).revision,profile(-6)); library.reload();
    fails([&] { fresh.verify_assignment(); }); // Binding unchanged, but its selected saved profile changed.
    require(fresh.current()->editor()->working()==profile(-3),"External profile update replaced an existing draft");
}
void legacy_migration(const std::filesystem::path& directory) {
    const auto path=directory/L"legacy.library";
    ProfileLibrary seed(path); const auto a=seed.create("Legacy",profile(-4)); auto legacy=read(path);
    const auto header_end=legacy.find('\n'); legacy.erase(header_end-2,2); legacy.replace(0,std::string("CONTOUR_LIBRARY 2").size(),"CONTOUR_LIBRARY 1"); write(path,legacy);
    ProfileLibrary library(path); require(library.get(a.id)==a && library.assignments().empty() && read(path)==legacy,"Read-only v1 open rewrote/lost profiles");
    ProfileWorkspace workspace(library,L"output");
    { FileLock lock(path); fails([&] { workspace.select(a.id); }); }
    require(read(path)==legacy && !workspace.current() && library.assignments().empty(),"Failed upgrade partly persisted an assignment");
    workspace.select(a.id); require(read(path).starts_with("CONTOUR_LIBRARY 2 "),"Successful mutation did not upgrade library");
    ProfileLibrary reopened(path); require(reopened.get(a.id)==a && reopened.assignment(L"output")->profile_id==a.id,"Migration changed profile identity/revision/EQ");
    const auto disk=read(path);
    for (const auto& id : {std::wstring{},std::wstring(L"x\0y",3),std::wstring(L"x\ny"),std::wstring(1025,L'a'),std::wstring(1,static_cast<wchar_t>(0xd800))}) {
        fails([&] { ProfileWorkspace invalid(library,id); });
    }
    require(read(path)==disk,"Invalid endpoint identity changed the library");
}
void corrupt_assignments(const std::filesystem::path& directory) {
    const auto path=directory/L"bad-assignment.library";
    ProfileLibrary library(path); const auto a=library.create("A",profile(-3)); ProfileWorkspace workspace(library,L"endpoint-A"); workspace.select(a.id);
    const auto good=read(path); const auto entries=library.entries(); const auto assignments=library.assignments();
    const auto start=good.find("Assignment "),end=good.find('\n',start),header_end=good.find('\n');
    const auto line=good.substr(start,end-start+1); std::vector<std::string> malformed;
    auto bad=good; bad.replace(start,end-start+1,"Assignment \"endpoint-A\" \"11111111-1111-1111-1111-111111111111\" 1\n"); malformed.push_back(bad);
    bad=good; bad.replace(end-1,1,"0"); malformed.push_back(bad);
    bad=good; bad.replace(end-1,1,"18446744073709551616"); malformed.push_back(bad);
    bad=good; bad.replace(good.find("endpoint-A",start),10,""); malformed.push_back(bad);
    bad=good; bad.replace(good.find("endpoint-A",start),10,std::string("\xff")); malformed.push_back(bad);
    bad=good; bad.replace(good.find("endpoint-A",start),10,std::string(1025,'x')); malformed.push_back(bad);
    bad=good; bad.replace(header_end-1,1,"2"); bad.insert(bad.find("EndLibrary"),line); malformed.push_back(bad);
    bad=good; bad.replace(header_end-1,1,"129"); malformed.push_back(bad);
    malformed.push_back(good.substr(0,start));
    for (const auto& bytes : malformed) {
        write(path,bytes); fails([&] { ProfileLibrary broken(path); }); fails([&] { library.reload(); }); fails([&] { workspace.save(); });
        require(read(path)==bytes && library.entries()==entries && library.assignments()==assignments,"Invalid assignment was silently repaired or partly loaded");
    }
    write(path,good); library.reload(); require(library.assignments()==assignments,"Repaired assignment file could not be reopened");
}
}
int wmain(int argc,wchar_t* argv[]) {
    if (argc!=2) { return 2; }
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if (FAILED(com)) { return 2; }
    const auto directory=std::filesystem::path(argv[1])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    test::Suite suite;
    suite.run("durable identity/name/format, independent duplicates, Unicode names, invalid create",[&] { storage(directory); });
    suite.run("Save/Don't save/Cancel switching, close guard, and per-profile undo histories",[&] { switching(directory); });
    suite.run("duplicate working edits, rename, Save as source restoration, independent Save/Revert",[&] { copies(directory); });
    suite.run("real atomic replacement and writer-lock failures preserve library, selection, and draft",[&] { failed_writes(directory); });
    suite.run("multiple writers, explicit reload, stale draft protection, and conflict rescue",[&] { conflicts(directory); });
    suite.run("corrupt/truncated/unsupported library fails closed and preserves prior state",[&] { corruption(directory); });
    suite.run("profile switching with independent preview identities and measured DSP output",[&] { preview_binding(directory); });
    suite.run("exact device identities, independent assignments, durable switches/Save as, read-only restore",[&] { remembered_devices(directory); });
    suite.run("failed bound switch/Save as preserves source checkpoint, draft, selection, and assignment",[&] { assignment_failures(directory); });
    suite.run("stale assigned/unassigned sessions, external writers, and stale selected profile detection",[&] { assignment_conflicts(directory); });
    suite.run("read-only v1 opening, atomic lazy v2 migration, invalid endpoint identities",[&] { legacy_migration(directory); });
    suite.run("dangling/duplicate/invalid assignments fail closed without partial load or writes",[&] { corrupt_assignments(directory); });
    const auto result=suite.finish(); CoUninitialize(); return result;
}
