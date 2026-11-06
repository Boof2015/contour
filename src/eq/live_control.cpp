#include <contour/live_control.h>
#include <propvarutil.h>
#include <bcrypt.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace contour::eq {
static_assert(sizeof(GUID)==16);
namespace {
void check(bool condition, const char* message) { if (!condition) { throw std::invalid_argument(message); } }
void hr(HRESULT result, const char* operation) {
    if (FAILED(result)) { throw std::runtime_error(std::string(operation)+"; HRESULT "+std::to_string(result)); }
}
GUID identity() { GUID id{}; hr(CoCreateGuid(&id),"Create control identity"); return id; }
void number(std::string& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i=0; i<bytes; ++i) { out.push_back(static_cast<char>((value>>(i*8))&0xff)); }
}
void guid(std::string& out, const GUID& value) { out.append(reinterpret_cast<const char*>(&value),sizeof(value)); }
void string(std::string& out, std::string_view value) { number(out,value.size(),4); out.append(value); }
struct Reader {
    std::string_view input;
    std::string_view take(std::size_t count) {
        check(count <= input.size(),"Truncated control message");
        const auto value = input.substr(0,count); input.remove_prefix(count); return value;
    }
    std::uint64_t number(unsigned bytes) {
        const auto value = take(bytes); std::uint64_t result = 0;
        for (unsigned i=0; i<bytes; ++i) { result |= static_cast<std::uint64_t>(static_cast<unsigned char>(value[i]))<<(i*8); }
        return result;
    }
    GUID guid() { GUID value{}; const auto bytes = take(sizeof(value)); std::memcpy(&value,bytes.data(),sizeof(value)); return value; }
    std::string string(std::size_t limit) { const auto count = number(4); check(count<=limit,"Oversized control field"); return std::string(take(static_cast<std::size_t>(count))); }
    void end() { check(input.empty(),"Trailing control data"); }
};
void valid(const PreviewRequest& r) {
    check(r.runtime != GUID_NULL && r.session != GUID_NULL && r.revision != 0,"Missing preview identity/revision");
    check(!r.profile.empty() && r.profile.size()<=1024*1024,"Invalid preview profile size");
}
void valid(const PreviewStatus& s) {
    check(s.runtime != GUID_NULL,"Missing runtime identity");
    check(static_cast<unsigned>(s.state)<=static_cast<unsigned>(PreviewState::stopped),"Unknown preview state");
    check(std::isfinite(s.sample_rate) && s.sample_rate>=8000 && s.sample_rate<=384000 && s.channels>=1 && s.channels<=dsp::max_channels,
          "Invalid runtime format");
    check(s.detail.size()<=2048,"Oversized preview diagnostic");
    check(static_cast<unsigned>(s.evidence)<=static_cast<unsigned>(RuntimeEvidence::transitioning),"Unknown runtime evidence");
    check(s.evidence!=RuntimeEvidence::unknown || !s.active_fingerprint,"Unknown runtime carries a fingerprint");
    check((s.evidence!=RuntimeEvidence::loaded && s.evidence!=RuntimeEvidence::processed) || s.active_fingerprint.has_value(),"Missing active processing fingerprint");
    check((s.session == GUID_NULL) == (s.revision == 0),"Incomplete preview acknowledgement");
    if (s.state == PreviewState::queued || s.state == PreviewState::applied) {
        check(s.revision != 0,"Missing preview acknowledgement");
    }
}
struct Variant { PROPVARIANT value{}; ~Variant() { PropVariantClear(&value); } };
} // namespace

ProcessingFingerprint processing_fingerprint(const Configuration& configuration,double rate,std::size_t channels) {
    ParametricEq compiled; compiled.configure(configuration,rate,channels);
    std::string bytes="ContourProcessing1"; number(bytes,std::bit_cast<std::uint64_t>(rate),8); number(bytes,channels,4);
    for (std::size_t channel=0;channel<channels;++channel) {
        const auto sections=compiled.coefficients(channel); number(bytes,sections.size(),4);
        for (const auto& c : sections) {
            for (const auto value : {c.b0,c.b1,c.b2,c.a1,c.a2}) {
                number(bytes,std::bit_cast<std::uint64_t>(value==0 ? 0.0 : value),8);
            }
        }
    }
    ProcessingFingerprint fingerprint{};
    const auto result=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),fingerprint.data(),static_cast<ULONG>(fingerprint.size()));
    if (result<0) { throw std::runtime_error("Cannot fingerprint audio configuration; NTSTATUS "+std::to_string(result)); }
    return fingerprint;
}

std::string encode_request(const PreviewRequest& r) {
    valid(r); std::string out="CTREQ001";
    guid(out,r.runtime); guid(out,r.session); number(out,r.revision,8); string(out,r.profile); return out;
}
PreviewRequest decode_request(std::string_view bytes) {
    check(bytes.size()<=max_request_bytes,"Oversized preview request"); Reader in{bytes};
    check(in.take(8)=="CTREQ001","Unsupported preview request version");
    PreviewRequest r; r.runtime=in.guid(); r.session=in.guid(); r.revision=in.number(8); r.profile=in.string(1024*1024);
    in.end(); valid(r); return r;
}
std::string encode_status(const PreviewStatus& s) {
    valid(s); std::string out="CTSTA002";
    guid(out,s.runtime); guid(out,s.session); number(out,s.revision,8); number(out,static_cast<unsigned>(s.state),4);
    number(out,std::bit_cast<std::uint64_t>(s.sample_rate),8); number(out,s.channels,4); number(out,s.heartbeat_ms,8);
    string(out,s.detail); number(out,static_cast<unsigned>(s.evidence),4); number(out,s.active_fingerprint ? 1 : 0,4);
    if (s.active_fingerprint) { out.append(reinterpret_cast<const char*>(s.active_fingerprint->data()),s.active_fingerprint->size()); }
    return out;
}
PreviewStatus decode_status(std::string_view bytes) {
    check(bytes.size()<=max_status_bytes,"Oversized preview status"); Reader in{bytes};
    const auto version=in.take(8); check(version=="CTSTA001" || version=="CTSTA002","Unsupported preview status version");
    PreviewStatus s; s.runtime=in.guid(); s.session=in.guid(); s.revision=in.number(8);
    s.state=static_cast<PreviewState>(in.number(4)); s.sample_rate=std::bit_cast<double>(in.number(8));
    s.channels=static_cast<std::uint32_t>(in.number(4)); s.heartbeat_ms=in.number(8); s.detail=in.string(2048);
    if (version=="CTSTA002") {
        s.evidence=static_cast<RuntimeEvidence>(in.number(4)); const auto present=in.number(4); check(present<=1,"Invalid fingerprint presence flag");
        if (present) { ProcessingFingerprint fingerprint{}; const auto data=in.take(fingerprint.size()); std::memcpy(fingerprint.data(),data.data(),data.size()); s.active_fingerprint=fingerprint; }
    }
    in.end(); valid(s); return s;
}
bool status_is_live(const PreviewStatus& status, std::uint64_t now) noexcept {
    return status.state != PreviewState::stopped && now>=status.heartbeat_ms && now-status.heartbeat_ms<=control_timeout_ms;
}

namespace {
HRESULT open_store(IMMDevice* endpoint, bool saved, DWORD access, IPropertyStore** store) noexcept {
    if (!store) { return E_POINTER; } *store=nullptr;
    if (!endpoint) { return E_POINTER; }
    PROPVARIANT context{}; context.vt=VT_CLSID; context.puuid=const_cast<GUID*>(&control_context); // Borrowed.
    Microsoft::WRL::ComPtr<IAudioSystemEffectsPropertyStore> effects;
    const auto result=endpoint->Activate(__uuidof(IAudioSystemEffectsPropertyStore),CLSCTX_ALL,&context,
        reinterpret_cast<void**>(effects.GetAddressOf()));
    return FAILED(result) ? result : saved ? effects->OpenUserPropertyStore(access,store) : effects->OpenVolatilePropertyStore(access,store);
}
HRESULT open_store(IMMDeviceCollection* devices, bool saved, DWORD access, IPropertyStore** store) noexcept {
    if (!store) { return E_POINTER; } *store=nullptr;
    if (!devices) { return E_NOTFOUND; }
    UINT count=0; auto result=devices->GetCount(&count);
    if (FAILED(result)) { return result; } if (count==0) { return E_NOTFOUND; }
    Microsoft::WRL::ComPtr<IMMDevice> endpoint;
    result=devices->Item(count-1,&endpoint);
    return FAILED(result) ? result : open_store(endpoint.Get(),saved,access,store);
}
} // namespace
HRESULT open_control_store(IMMDevice* endpoint, IPropertyStore** store) noexcept { return open_store(endpoint,false,STGM_READWRITE,store); }
HRESULT open_control_store(IMMDeviceCollection* devices, IPropertyStore** store) noexcept { return open_store(devices,false,STGM_READWRITE,store); }
HRESULT open_saved_store(IMMDevice* endpoint, DWORD access, IPropertyStore** store) noexcept { return open_store(endpoint,true,access,store); }
HRESULT open_saved_store(IMMDeviceCollection* devices, DWORD access, IPropertyStore** store) noexcept { return open_store(devices,true,access,store); }
std::optional<std::string> read_control_value(IPropertyStore& store, REFPROPERTYKEY key, std::size_t limit) {
    Variant v; hr(store.GetValue(key,&v.value),"Read effects property");
    if (v.value.vt==VT_EMPTY) { return {}; }
    check(v.value.vt==(VT_VECTOR|VT_UI1),"Unexpected preview property type");
    check(v.value.caub.cElems<=limit && (v.value.caub.cElems==0 || v.value.caub.pElems),"Invalid preview property size");
    if (v.value.caub.cElems==0) { return std::string{}; }
    return std::string(reinterpret_cast<const char*>(v.value.caub.pElems),v.value.caub.cElems);
}
void write_control_value(IPropertyStore& store, REFPROPERTYKEY key, std::string_view bytes) {
    check(bytes.size()<=max_request_bytes,"Oversized preview property");
    Variant v; hr(InitPropVariantFromBuffer(bytes.data(),static_cast<UINT>(bytes.size()),&v.value),"Encode preview property");
    hr(store.SetValue(key,v.value),"Write effects property"); hr(store.Commit(),"Commit effects property");
}

std::optional<Profile> read_saved_profile(IPropertyStore& store) {
    const auto bytes=read_control_value(store,saved_profile_key,max_saved_profile_bytes);
    return bytes ? std::optional{parse_profile(*bytes)} : std::nullopt;
}
void write_saved_profile(IPropertyStore& store, const Profile& profile) {
    const auto bytes=serialize_profile(profile);
    check(bytes.size()<=max_saved_profile_bytes,"Saved profile is too large");
    write_control_value(store,saved_profile_key,bytes);
    check(read_control_value(store,saved_profile_key,max_saved_profile_bytes)==std::optional{bytes},
          "Saved profile readback differs; reload settings before retrying");
}
void save_profile_settings(EditSession& editor, IPropertyStore& store) {
    editor.save_with([&](const Profile& profile) { write_saved_profile(store,profile); });
}

PreviewProcessor::PreviewProcessor(SmoothEq& runtime, double rate, std::size_t channels, std::string startup_detail,
    std::optional<ProcessingFingerprint> initial_fingerprint) : runtime_(runtime),active_revision_(runtime.applied_revision()),active_fingerprint_(std::move(initial_fingerprint)) {
    check(channels>=1 && channels<=dsp::max_channels,"Invalid runtime channel count");
    status_.runtime=identity(); status_.sample_rate=rate; status_.channels=static_cast<std::uint32_t>(channels);
    status_.detail=startup_detail.substr(0,2048);
    status_.heartbeat_ms=GetTickCount64(); bank_revision_=runtime.applied_revision(); valid(status_);
}
void PreviewProcessor::submit(std::string_view bytes) {
    if (status_.state==PreviewState::stopped) { return; }
    PreviewRequest request;
    try { request=decode_request(bytes); }
    catch (const std::exception& e) {
        status_.state=PreviewState::rejected; status_.session=GUID_NULL; status_.revision=0; status_.detail=e.what(); return;
    }
    if (request.runtime!=status_.runtime) { return; }
    if (request.session==last_session_ && request.revision<=last_request_revision_) { return; }
    last_session_=request.session; last_request_revision_=request.revision;
    status_.session=request.session; status_.revision=request.revision; status_.detail.clear();
    try {
        const auto profile=parse_profile(request.profile);
        check(profile.channels==status_.channels,"Preview channel count does not match runtime");
        check(bank_revision_<SmoothEq::transitioning-1,"Audio revision exhausted");
        std::optional<ProcessingFingerprint> fingerprint;
        try { fingerprint=processing_fingerprint(profile.configuration,status_.sample_rate,status_.channels); } catch (...) {} // Diagnostic failure must not reject valid audio.
        auto* bank=ResidentBank::prepare(profile.configuration,status_.sample_rate,status_.channels,bank_revision_+1);
        check(runtime_.publish(bank),"Audio runtime rejected preview bank");
        ++bank_revision_; pending_revision_=bank_revision_; pending_fingerprint_=fingerprint; status_.state=PreviewState::queued;
    } catch (const std::exception& e) { status_.state=PreviewState::rejected; status_.detail=e.what(); }
}
const PreviewStatus& PreviewProcessor::poll() {
    runtime_.reclaim(); status_.heartbeat_ms=GetTickCount64();
    if (status_.state==PreviewState::stopped) { return status_; }
    const auto processed=runtime_.processed_revision(),applied=runtime_.applied_revision();
    if (pending_revision_ && applied==*pending_revision_) {
        active_revision_=applied; active_fingerprint_=pending_fingerprint_; pending_revision_.reset(); pending_fingerprint_.reset();
    }
    status_.evidence=RuntimeEvidence::unknown; status_.active_fingerprint.reset();
    if (processed==SmoothEq::transitioning || (pending_revision_ && applied!=*pending_revision_)) {
        status_.evidence=RuntimeEvidence::transitioning;
    } else if (active_revision_==applied && active_fingerprint_) {
        if (processed==SmoothEq::not_processed) { status_.evidence=RuntimeEvidence::loaded; }
        else if (processed==applied) { status_.evidence=RuntimeEvidence::processed; }
        if (status_.evidence!=RuntimeEvidence::unknown) { status_.active_fingerprint=active_fingerprint_; }
    }
    if (status_.state==PreviewState::queued && applied==bank_revision_) { status_.state=PreviewState::applied; }
    return status_;
}
const PreviewStatus& PreviewProcessor::stop() noexcept {
    status_.evidence=RuntimeEvidence::unknown; status_.active_fingerprint.reset();
    status_.state=PreviewState::stopped; status_.heartbeat_ms=GetTickCount64(); return status_;
}

LivePreview::LivePreview(EditSession& editor, IPropertyStore* store, std::optional<bool> bypass_override)
    : editor_(editor),bypass_override_(bypass_override),store_(store) {
    check(store!=nullptr,"Missing preview property store");
}
std::optional<PreviewStatus> LivePreview::read_status() {
    const auto bytes=read_control_value(*store_.Get(),preview_status_key,max_status_bytes);
    return bytes ? std::optional{decode_status(*bytes)} : std::nullopt;
}
void LivePreview::disconnect() noexcept { runtime_id_=GUID_NULL; pending_.reset(); editor_.clear_applied(); }
PreviewStatus LivePreview::connect() {
    disconnect(); const auto status=read_status();
    check(status && status_is_live(*status,GetTickCount64()),"No responsive Contour audio runtime");
    check(status->channels==editor_.working().channels,"Profile channel count does not match audio runtime");
    session_id_=identity(); runtime_id_=status->runtime; return *status;
}
void LivePreview::submit() {
    try {
        const auto status=read_status();
        check(runtime_id_!=GUID_NULL && status && status->runtime==runtime_id_ && status_is_live(*status,GetTickCount64()),
              "Audio runtime changed or stopped; reconnect before previewing");
        const auto snapshot=editor_.snapshot();
        auto processing=snapshot->profile();
        if (bypass_override_) { processing.configuration.bypass=*bypass_override_; }
        const auto bytes=encode_request({runtime_id_,session_id_,snapshot->revision(),serialize_profile(processing)});
        write_control_value(*store_.Get(),preview_request_key,bytes); pending_=snapshot;
    } catch (...) { disconnect(); throw; }
}
PreviewReply LivePreview::poll() {
    try {
        const auto status=read_status();
        if (runtime_id_==GUID_NULL || !status || status->runtime!=runtime_id_ || !status_is_live(*status,GetTickCount64())) {
            disconnect(); return {PreviewFeedback::disconnected,"Audio runtime stopped or changed"};
        }
        if (!pending_) { return {PreviewFeedback::ready,status->detail}; }
        if (status->session!=GUID_NULL && status->session!=session_id_ && editor_.applied()) {
            disconnect(); return {PreviewFeedback::disconnected,"Another editor changed the audio settings"};
        }
        if (status->session==session_id_ && status->revision==pending_->revision()) {
            if (status->state==PreviewState::applied) {
                editor_.mark_applied(pending_); return {PreviewFeedback::applied,{}};
            }
            if (status->state==PreviewState::rejected) { return {PreviewFeedback::rejected,status->detail}; }
        }
        return {PreviewFeedback::waiting,{}};
    } catch (const std::exception& e) { disconnect(); return {PreviewFeedback::disconnected,e.what()}; }
}
} // namespace contour::eq
