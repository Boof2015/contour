#include <contour/edit_session.h>
#include <contour/live_eq.h>
#include <contour/peq_import.h>
#include "test_support.h"
#include <windows.h>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using namespace contour::eq;
using test::require;

namespace {
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}

Configuration gain(double db) { return {{{"Trim", {}, {}, db}}}; }

void validation() {
    EditSession session({{},48000,2});
    const auto initial = session.snapshot();
    require(!session.dirty() && !session.applied() && session.awaiting_preview(), "new session claimed active audio");
    require(session.mark_applied(initial) && !session.awaiting_preview(), "initial acknowledgement lost");
    require(!session.replace({}) && !session.can_undo(), "no-op created an edit");
    Configuration valid{{{"Left", ChannelSelection::selected(1), {{FilterType::peak,1000,-6,2}}}}};
    require(session.replace(valid), "valid edit rejected");
    const auto edited = session.snapshot();
    require(session.dirty() && session.awaiting_preview(), "edit did not separate working from saved/applied");
    require(initial->profile().configuration.layers.empty(), "edit mutated an earlier snapshot");

    std::vector<Configuration> invalid;
    auto bad = valid; bad.layers[0].filters[0].frequency_hz = 24000; invalid.push_back(bad);
    bad = valid; bad.layers[0].filters[0].q = 0; invalid.push_back(bad);
    bad = valid; bad.layers[0].filters[0].gain_db = std::numeric_limits<double>::quiet_NaN(); invalid.push_back(bad);
    bad = valid; bad.layers[0].channels.mask = 4; invalid.push_back(bad);
    bad = valid; bad.layers[0].name = "two\nlines"; invalid.push_back(bad);
    bad = valid; bad.layers[0].filters.resize(max_filters_per_layer+1); invalid.push_back(bad);
    bad = valid; bad.layers.resize(max_layers+1); invalid.push_back(bad);
    // Per-layer counts are valid, but their overlapping channel exceeds the DSP budget.
    bad = valid; bad.layers[0].filters.resize(max_filters_per_layer, valid.layers[0].filters[0]);
    bad.layers.resize(5, bad.layers[0]); invalid.push_back(bad);
    for (const auto& configuration : invalid) {
        rejects([&] { session.replace(configuration); });
        require(session.snapshot() == edited && session.applied() == initial && session.saved() == initial->profile(),
                "invalid edit changed saved, working, or applied state");
    }
    require(session.undo() && !session.dirty() && session.can_redo(), "invalid edits corrupted undo history");
    const auto undone = session.snapshot();
    rejects([&] { session.replace(invalid[0]); });
    require(session.snapshot() == undone && session.can_redo(), "invalid edit discarded redo");
    require(session.redo() && session.working().configuration == valid, "redo lost valid settings");
    require(!session.replace(valid), "identical replacement created another revision");
}

void gestures() {
    EditSession session({gain(-1),48000,2});
    session.begin_edit(); session.end_edit();
    require(!session.can_undo(), "empty gesture created history");
    session.begin_edit();
    for (int i = 2; i <= 10; ++i) {
        session.begin_edit(); // Repeated begin must not split a drag.
        require(session.replace(gain(-i)), "gesture value rejected");
    }
    const auto before_rejection = session.snapshot();
    rejects([&] { session.replace(gain(37)); });
    require(session.snapshot() == before_rejection, "failed gesture value changed working settings");
    session.end_edit(); session.end_edit();
    require(session.undo() && !session.dirty() && !session.can_undo(), "drag did not undo in one step");
    require(session.redo() && session.working().configuration == gain(-10), "drag did not redo in one step");
    session.begin_edit();
    session.replace(gain(-12)); session.replace(gain(-10));
    session.end_edit();
    require(session.undo() && !session.dirty() && !session.can_undo(), "round-trip gesture left an empty undo step");
    session.replace(gain(-7));
    require(!session.can_redo(), "branch edit retained stale redo");
    session.begin_edit(); session.replace(gain(-8));
    require(session.undo() && session.working().configuration == gain(-7), "undo during a drag lost its starting value");
    require(session.redo() && session.working().configuration == gain(-8), "redo during a drag lost its final value");
    session.replace(gain(-9));
    require(session.undo() && session.working().configuration == gain(-8), "undo left gesture grouping open");
}

void checkpoints(const std::filesystem::path& directory) {
    const auto path = directory/L"checkpoint.contour";
    EditSession session({gain(-1),48000,2});
    session.save(path);
    const auto original_file = read_profile_file(path);
    session.mark_applied(session.snapshot());
    session.replace(gain(-4));
    require(read_profile_file(path) == original_file, "audition overwrote the saved profile");
    const auto revision = session.snapshot();
    session.save(path);
    require(!session.dirty() && session.snapshot() == revision && session.awaiting_preview(),
            "save changed the edit revision or falsely acknowledged audio");
    require(session.undo() && session.dirty(), "save destroyed undo or kept the old checkpoint");
    require(session.redo() && !session.dirty(), "redo to saved settings remained dirty");
    session.replace(gain(-9));
    const auto before_revert = session.snapshot();
    require(session.revert() && !session.dirty() && !session.can_undo() && !session.can_redo(), "revert failed");
    require(session.snapshot()->revision() > before_revert->revision() && session.awaiting_preview(),
            "revert did not create a new update for the audio runtime");
    require(!session.revert(), "clean revert created an edit");
    const auto text = read_profile_file(path);
    require(text.has_value(), "saved profile missing");
    EditSession reopened(parse_profile(*text));
    require(reopened.working() == session.saved() && !reopened.dirty() && !reopened.applied(), "reopen lost checkpoint");
    rejects([&] { reopened.mark_applied(session.snapshot()); });
}

void failed_save(const std::filesystem::path& directory) {
    const auto path = directory/L"locked.contour";
    EditSession session({gain(-2),48000,2});
    session.save(path);
    const auto original = read_profile_file(path);
    session.replace(gain(-8));
    const auto working = session.snapshot();
    // A real Windows sharing failure during atomic replacement, after the temp
    // file has been written, must leave both the checkpoint and original intact.
    struct Handle {
        HANDLE value;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
    } lock{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    require(lock.value != INVALID_HANDLE_VALUE, "could not lock fixture");
    bool failed = false;
    try { session.save(path); } catch (const std::runtime_error&) { failed = true; }
    require(failed && session.dirty() && session.saved().configuration == gain(-2), "failed save changed checkpoint");
    require(session.snapshot() == working && read_profile_file(path) == original, "failed save changed working/file");
    require(session.undo() && !session.dirty(), "failed save changed history");
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        require(!entry.path().filename().wstring().starts_with(L"locked.contour."), "failed save left a transaction file");
    }
}

void acknowledgements() {
    EditSession session({gain(-1),48000,2});
    const auto first = session.snapshot();
    session.replace(gain(-3)); const auto second = session.snapshot();
    session.replace(gain(-6)); const auto third = session.snapshot();
    require(session.mark_applied(second), "older in-flight revision was not accepted");
    require(session.applied()->profile().configuration == gain(-3) && session.awaiting_preview(),
            "in-flight acknowledgement claimed the latest edit was active");
    require(!session.mark_applied(first) && session.applied() == second, "late acknowledgement moved audio state backwards");
    require(session.mark_applied(third) && !session.awaiting_preview(), "latest acknowledgement did not converge");
    require(!session.mark_applied(third), "duplicate acknowledgement changed state");
    require(session.undo() && session.awaiting_preview(), "undo did not need a new audio revision");
    require(!session.mark_applied(second) && session.applied() == third, "old matching settings acknowledged a new undo");
    EditSession other(session.working());
    rejects([&] { session.mark_applied(other.snapshot()); });
    rejects([&] { session.mark_applied({}); });
}

void history_limit() {
    EditSession session({gain(0),48000,2});
    for (std::size_t i = 1; i <= EditSession::max_history+2; ++i) { session.replace(gain(-static_cast<double>(i)/10)); }
    const auto last = session.working().configuration;
    session.begin_edit(); session.replace(gain(-30)); session.replace(last); session.end_edit();
    std::size_t steps = 0;
    while (session.undo()) { ++steps; }
    require(steps == EditSession::max_history, "history limit or no-op gesture discarded wrong entries");
    require(session.working().configuration == gain(-0.2), "bounded undo reached wrong settings");
    while (session.redo()) { --steps; }
    require(steps == 0 && session.working().configuration == last, "bounded redo lost edits");
    require(session.revert() && !session.dirty(), "saved checkpoint was lost when history was trimmed");
}

// The following exercises the actual processing runtime in memory. It never
// attaches to an endpoint, opens an audio stream, or plays the generated tone.
void publish(SmoothEq& runtime, const EditSession::Snapshot& snapshot) {
    const auto& p = snapshot->profile();
    require(runtime.publish(ResidentBank::prepare(p.configuration,p.validation_sample_rate,p.channels,snapshot->revision())),
            "runtime rejected a validated edit");
}

void confirm(EditSession& session, SmoothEq& runtime, const EditSession::Snapshot& snapshot) {
    require(runtime.applied_revision() == snapshot->revision(), "audio has not completed the requested update");
    require(session.mark_applied(snapshot), "audio acknowledgement rejected");
    runtime.reclaim();
}

void measure(SmoothEq& runtime, double left_db, double right_db) {
    constexpr std::size_t warmup = 4800, count = 4800, total = warmup+count;
    std::vector<float> input(total*2), output(input.size());
    for (std::size_t i = 0; i < total; ++i) {
        const auto value = static_cast<float>(0.01*std::sin(2*std::numbers::pi*1000*static_cast<double>(i)/48000));
        input[2*i] = input[2*i+1] = value;
    }
    runtime.process(input.data(),output.data(),total);
    for (std::size_t c = 0; c < 2; ++c) {
        double source = 0, processed = 0;
        for (std::size_t i = warmup; i < total; ++i) {
            const double x = input[2*i+c], y = output[2*i+c];
            require(std::isfinite(y), "nonfinite output after edit");
            source += x*x; processed += y*y;
        }
        const double db = 10*std::log10(processed/source), expected = c ? right_db : left_db;
        require(std::isfinite(db) && std::abs(db-expected) < 1e-4, "measured channel EQ differs from expected response");
    }
}

void audition(const std::filesystem::path& directory) {
    EditSession session({{},48000,2});
    SmoothEq runtime;
    runtime.initialize(ResidentBank::prepare({},48000,2,0),960);
    publish(runtime,session.snapshot()); measure(runtime,0,0); confirm(session,runtime,session.snapshot());

    auto left = import_peq_text("Preamp: -1 dB\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 2\n");
    auto right = import_peq_text("Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 2\n");
    require(left.layer && right.layer, "fixture import failed");
    left.layer->name = "Left"; left.layer->channels = ChannelSelection::selected(1);
    right.layer->name = "Right"; right.layer->channels = ChannelSelection::selected(2);
    Configuration configuration{{*left.layer,*right.layer,{"All",{},{{FilterType::peak,1000,1,1}},-1}}};
    session.replace(configuration); publish(runtime,session.snapshot());
    configuration.layers.back().gain_db = -2;
    session.replace(configuration);
    const auto corrected = session.snapshot();
    publish(runtime,corrected); // Coalesces the earlier unpublished-to-audio edit.
    std::array<float,256> buffer{};
    runtime.process(buffer.data(),buffer.data(),128);
    require(runtime.applied_revision() != corrected->revision() && session.awaiting_preview(),
            "queueing/beginning a crossfade was reported as fully applied");
    measure(runtime,-8,-4); confirm(session,runtime,corrected);
    require(session.dirty(), "audition changed the saved checkpoint");
    const auto path = directory/L"audition.contour";
    session.save(path);
    const auto saved = read_profile_file(path);

    // An imported band is edited through exactly the same configuration path.
    configuration.layers[0].filters[0].gain_db = -12;
    session.replace(configuration); publish(runtime,session.snapshot());
    measure(runtime,-14,-4); confirm(session,runtime,session.snapshot());
    require(read_profile_file(path) == saved, "manual edit overwrote saved imported EQ");
    require(session.undo(), "manual edit could not be undone");
    publish(runtime,session.snapshot()); measure(runtime,-8,-4); confirm(session,runtime,session.snapshot());
    require(session.redo(), "manual edit could not be redone");
    publish(runtime,session.snapshot()); measure(runtime,-14,-4); confirm(session,runtime,session.snapshot());
    require(session.revert(), "manual edit could not be reverted");
    publish(runtime,session.snapshot()); measure(runtime,-8,-4); confirm(session,runtime,session.snapshot());

    configuration = session.working().configuration; configuration.bypass = true;
    session.replace(configuration); publish(runtime,session.snapshot());
    measure(runtime,0,0); confirm(session,runtime,session.snapshot());
    require(session.revert(), "bypass could not be reverted");
    publish(runtime,session.snapshot()); measure(runtime,-8,-4); confirm(session,runtime,session.snapshot());
    require(!session.dirty() && !session.awaiting_preview(), "saved, working, and applied state did not converge");
    require(parse_profile(*read_profile_file(path)) == session.working(), "persisted settings differ from auditioned checkpoint");
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) { return 2; }
    const auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    test::Suite suite;
    suite.run("atomic edits, engine validation, immutable snapshots, and no-op edits",validation);
    suite.run("grouped drag history, undo/redo, failed values, and branching",gestures);
    suite.run("save checkpoint, live draft separation, revert, and reopen",[&] { checkpoints(directory); });
    suite.run("failed atomic save preserves checkpoint, working state, and original file",[&] { failed_save(directory); });
    suite.run("asynchronous, stale, duplicate, and foreign-session audio acknowledgements",acknowledgements);
    suite.run("bounded undo/redo preserves the saved checkpoint",history_limit);
    suite.run("import, edit, audition, save, undo/redo, revert, and bypass through the DSP runtime",[&] { audition(directory); });
    return suite.finish();
}
