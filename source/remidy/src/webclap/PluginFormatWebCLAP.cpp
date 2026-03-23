#ifdef __EMSCRIPTEN__

#include "PluginFormatWebCLAP.hpp"

#include <choc/text/choc_JSON.h>
#include <emscripten.h>
#include <iostream>
#include <sstream>
#include <umppi/umppi.hpp>

// Forward declarations: defined in WebAudioWorkletIODevice.cpp as EM_JS functions.
extern "C" void uapmd_post_to_webclap_worklet_json(const char* json);
extern "C" void uapmd_webclap_load_plugin_async(const char* json);

namespace remidy {

// Hardcoded plugin catalog — two known example bundles returned unconditionally.
// Plugin descriptors will eventually be discovered at runtime via the CLAP
// factory chain once a plugin is loaded.

struct KnownPlugin {
    const char* url;
    const char* displayName;
};

static constexpr KnownPlugin kKnownPlugins[] = {
    {
        "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/"
            "basics.wclap.tar.gz",
        "Signalsmith Basics"
    },
    {
        "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/"
            "example-plugins.wclap.tar.gz",
        "Signalsmith CLAP C++"
    },
};

// ── Shared runtime state for extern "C" callback ──────────────────────────────

struct WebClapGlobalState {
    std::atomic<uint32_t> next_slot{0};
    std::atomic<uint32_t> next_req_id{1};
    std::mutex pending_mutex;
    std::unordered_map<uint32_t, PluginFormatWebCLAPImpl::PendingRequest> pending_requests;
    std::mutex instances_mutex;
    std::unordered_map<uint32_t, PluginInstanceWebCLAP*> instances_by_slot;
    std::unordered_map<uint32_t, std::vector<WebClapParamDescriptor>> pending_parameters_by_slot;
};

static WebClapGlobalState& webclapGlobalState() {
    static WebClapGlobalState state;
    return state;
}

static void unregisterWebClapInstance(uint32_t slot) {
    auto& state = webclapGlobalState();
    std::lock_guard<std::mutex> lock(state.instances_mutex);
    state.instances_by_slot.erase(slot);
}

// Called from the EM_JS onmessage handler in WebAudioWorkletIODevice.cpp when
// the AudioWorklet sends a response (wclap-plugin-ready, wclap-plugin-error, …).
// The function must be exported so that the EM_JS can call it as
// _uapmd_webclap_on_worklet_message.

extern "C" EMSCRIPTEN_KEEPALIVE
void uapmd_webclap_on_worklet_message(const char* json) {
    PluginFormatWebCLAPImpl impl;
    impl.onWorkletMessage(json);
}

static std::vector<WebClapParamDescriptor> parseParamDescriptors(const choc::value::ValueView& value) {
    std::vector<WebClapParamDescriptor> descriptors;
    if (!value.isArray())
        return descriptors;

    auto size = value.size();
    descriptors.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        auto item = value[i];
        if (!item.isObject())
            continue;
        WebClapParamDescriptor descriptor;
        descriptor.index       = item["index"].getWithDefault<int32_t>(static_cast<int32_t>(i));
        descriptor.id          = item["id"].toString();
        descriptor.name        = item["name"].toString();
        descriptor.path        = item["path"].toString();
        descriptor.minValue    = item["minValue"].getWithDefault<double>(0.0);
        descriptor.maxValue    = item["maxValue"].getWithDefault<double>(1.0);
        descriptor.defaultValue= item["defaultValue"].getWithDefault<double>(0.0);
        descriptor.currentValue= item["currentValue"].getWithDefault<double>(descriptor.defaultValue);
        descriptor.automatable = item["automatable"].getWithDefault<bool>(false);
        descriptor.hidden      = item["hidden"].getWithDefault<bool>(false);
        descriptor.readOnly    = item["readOnly"].getWithDefault<bool>(false);
        descriptor.stepped     = item["stepped"].getWithDefault<bool>(false);
        descriptors.emplace_back(std::move(descriptor));
    }
    return descriptors;
}

static std::string buildWclapEventJson(const std::string& type,
                                       uint32_t slot,
                                       const std::string& payload)
{
    std::ostringstream json;
    json << "{\"type\":\"" << type << "\",\"slot\":" << slot;
    if (!payload.empty())
        json << "," << payload;
    json << "}";
    return json.str();
}

static std::string buildWclapBatchUmpJson(uint32_t slot, EventSequence& eventIn) {
    auto* bytes = static_cast<const uint8_t*>(eventIn.getMessages());
    const size_t bytes_available = eventIn.position();
    if (!bytes || bytes_available == 0)
        return {};

    std::ostringstream payload;
    payload << "\"events\":[";

    bool has_events = false;
    size_t offset = 0;
    while (offset + sizeof(uint32_t) <= bytes_available) {
        auto* words = reinterpret_cast<const uint32_t*>(bytes + offset);
        auto message_type = static_cast<uint8_t>(words[0] >> 28);
        auto word_count = umppi::umpSizeInInts(message_type);
        auto message_size = static_cast<size_t>(word_count) * sizeof(uint32_t);
        if (message_size == 0 || offset + message_size > bytes_available)
            break;

        if (has_events)
            payload << ",";
        has_events = true;
        payload << "{\"wordCount\":" << word_count << ",\"words\":[";
        for (uint8_t i = 0; i < word_count; ++i) {
            if (i != 0)
                payload << ",";
            payload << words[i];
        }
        payload << "]}";
        offset += message_size;
    }

    if (!has_events)
        return {};

    payload << "]";
    return buildWclapEventJson("wclap-send-ump-batch", slot, payload.str());
}

std::vector<PluginParameter*>& PluginInstanceWebCLAP::ParamSupportWebCLAP::parameters() {
    return owner_->parameterPointers();
}

std::vector<PluginParameter*>& PluginInstanceWebCLAP::ParamSupportWebCLAP::perNoteControllers(
    PerNoteControllerContextTypes, PerNoteControllerContext)
{
    return owner_->parameterPointers();
}

StatusCode PluginInstanceWebCLAP::ParamSupportWebCLAP::setParameter(
    uint32_t index, double plainValue, uint64_t)
{
    owner_->setCachedParameterValue(index, plainValue);
    std::ostringstream payload;
    payload << "\"index\":" << index << ",\"value\":" << plainValue;
    auto json = buildWclapEventJson("wclap-set-parameter", owner_->slot(), payload.str());
    uapmd_post_to_webclap_worklet_json(json.c_str());
    parameterChangeEvent().notify(index, plainValue);
    return StatusCode::OK;
}

StatusCode PluginInstanceWebCLAP::ParamSupportWebCLAP::getParameter(
    uint32_t index, double* plainValue)
{
    if (!plainValue)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    return owner_->getCachedParameterValue(index, plainValue)
        ? StatusCode::OK
        : StatusCode::NOT_IMPLEMENTED;
}

StatusCode PluginInstanceWebCLAP::ParamSupportWebCLAP::setPerNoteController(
    PerNoteControllerContext, uint32_t index, double plainValue, uint64_t timestamp)
{
    return setParameter(index, plainValue, timestamp);
}

std::string PluginInstanceWebCLAP::ParamSupportWebCLAP::valueToString(uint32_t index, double v) {
    return owner_->buildParameterValueString(index, v);
}

// ── PluginFormatWebCLAP::create ───────────────────────────────────────────────

std::unique_ptr<PluginFormatWebCLAP> PluginFormatWebCLAP::create() {
    return std::make_unique<PluginFormatWebCLAPImpl>();
}

// ── PluginFormatWebCLAPImpl ───────────────────────────────────────────────────

PluginFormatWebCLAPImpl::PluginFormatWebCLAPImpl() {
}

PluginFormatWebCLAPImpl::~PluginFormatWebCLAPImpl() {
}

void PluginFormatWebCLAPImpl::registerInstance(PluginInstanceWebCLAP* instance) {
    if (!instance)
        return;
    auto& state = webclapGlobalState();
    std::lock_guard<std::mutex> lock(state.instances_mutex);
    state.instances_by_slot[instance->slot()] = instance;
    auto it = state.pending_parameters_by_slot.find(instance->slot());
    if (it != state.pending_parameters_by_slot.end()) {
        instance->updateParameters(it->second);
        state.pending_parameters_by_slot.erase(it);
    }
}

void PluginFormatWebCLAPImpl::unregisterInstance(uint32_t slot) {
    unregisterWebClapInstance(slot);
}

void PluginFormatWebCLAPImpl::createInstance(
    PluginCatalogEntry* info,
    PluginInstantiationOptions /*options*/,
    std::function<void(std::unique_ptr<PluginInstance>, std::string)> callback)
{
    auto& state = webclapGlobalState();
    const uint32_t slot   = state.next_slot.fetch_add(1, std::memory_order_relaxed);
    const uint32_t req_id = state.next_req_id.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(state.pending_mutex);
        state.pending_requests.emplace(req_id, PendingRequest{info, slot, std::move(callback)});
    }

    // Fetch and compile the plugin WASM on the main thread (where fetch/URL are
    // available), then forward the compiled init object to the worklet.
    std::ostringstream json;
    json << "{\"type\":\"wclap-load-plugin\""
         << ",\"reqId\":"  << req_id
         << ",\"slot\":"   << slot
         << ",\"url\":\""  << info->pluginId() << "\""
         << "}";
    uapmd_webclap_load_plugin_async(json.str().c_str());
}

void PluginFormatWebCLAPImpl::onWorkletMessage(const char* json_cstr) {
    const auto message = choc::json::parse(json_cstr);
    const auto type = message["type"].toString();

    if (type == "wclap-plugin-ready") {
        const uint32_t req_id = static_cast<uint32_t>(message["reqId"].getWithDefault<int32_t>(0));
        const uint32_t slot   = static_cast<uint32_t>(message["slot"].getWithDefault<int32_t>(0));
        auto& state = webclapGlobalState();

        PendingRequest req;
        {
            std::lock_guard<std::mutex> lock(state.pending_mutex);
            auto it = state.pending_requests.find(req_id);
            if (it == state.pending_requests.end()) {
                std::cerr << "[WebCLAP] wclap-plugin-ready: unknown reqId "
                          << req_id << std::endl;
                return;
            }
            req = std::move(it->second);
            state.pending_requests.erase(it);
        }
        auto instance = std::make_unique<PluginInstanceWebCLAP>(req.entry, slot);
        registerInstance(instance.get());
        std::string no_error;
        req.callback(std::move(instance), no_error);

    } else if (type == "wclap-plugin-error") {
        const uint32_t req_id = static_cast<uint32_t>(message["reqId"].getWithDefault<int32_t>(0));
        std::string error = message["error"].toString();
        auto& state = webclapGlobalState();

        PendingRequest req;
        {
            std::lock_guard<std::mutex> lock(state.pending_mutex);
            auto it = state.pending_requests.find(req_id);
            if (it == state.pending_requests.end()) {
                std::cerr << "[WebCLAP] wclap-plugin-error: unknown reqId "
                          << req_id << std::endl;
                return;
            }
            req = std::move(it->second);
            state.pending_requests.erase(it);
        }
        req.callback(nullptr, error);
    } else if (type == "wclap-parameters") {
        const uint32_t slot = static_cast<uint32_t>(message["slot"].getWithDefault<int32_t>(0));
        auto descriptors = parseParamDescriptors(message["paramDescriptors"]);
        PluginInstanceWebCLAP* instance = nullptr;
        auto& state = webclapGlobalState();
        {
            std::lock_guard<std::mutex> lock(state.instances_mutex);
            auto it = state.instances_by_slot.find(slot);
            if (it != state.instances_by_slot.end())
                instance = it->second;
            else
                state.pending_parameters_by_slot[slot] = descriptors;
        }
        if (instance)
            instance->updateParameters(descriptors);
    } else if (type == "wclap-runtime-error") {
        std::cerr << "[WebCLAP] runtime error: " << message["error"].toString() << std::endl;
    }
    // All other message types (wclap-host-ready, etc.) are silently ignored here.
}

// ── PluginScanningWebCLAP ─────────────────────────────────────────────────────

std::vector<std::unique_ptr<PluginCatalogEntry>>
PluginScanningWebCLAP::scanAllAvailablePlugins(bool /*requireFastScanning*/) {
    std::vector<std::unique_ptr<PluginCatalogEntry>> result;
    std::string format_name = owner_->name();
    for (const auto& p : kKnownPlugins) {
        auto e = std::make_unique<PluginCatalogEntry>();
        e->format(format_name);
        std::string id{p.url};
        e->pluginId(id);
        e->displayName(p.displayName);
        result.emplace_back(std::move(e));
    }
    return result;
}

// ── PluginInstanceWebCLAP ─────────────────────────────────────────────────────

PluginInstanceWebCLAP::PluginInstanceWebCLAP(PluginCatalogEntry* entry, uint32_t slot)
    : PluginInstance(entry), slot_(slot)
{}

PluginInstanceWebCLAP::~PluginInstanceWebCLAP() {
    auto json = buildWclapEventJson("wclap-unload", slot_, "");
    uapmd_post_to_webclap_worklet_json(json.c_str());
    unregisterWebClapInstance(slot_);
}

void PluginInstanceWebCLAP::updateParameters(const std::vector<WebClapParamDescriptor>& descriptors) {
    std::lock_guard<std::mutex> lock(parameter_mutex_);

    parameter_defs_.clear();
    parameter_ptrs_.clear();
    parameter_values_.clear();
    parameter_defs_.reserve(descriptors.size());
    parameter_ptrs_.reserve(descriptors.size());

    for (const auto& descriptor : descriptors) {
        auto stable_id = descriptor.id;
        auto name = descriptor.name;
        auto path = descriptor.path;
        auto parameter = std::make_unique<PluginParameter>(
            descriptor.index,
            stable_id,
            name,
            path,
            descriptor.defaultValue,
            descriptor.minValue,
            descriptor.maxValue,
            descriptor.automatable,
            !descriptor.readOnly,
            descriptor.hidden,
            descriptor.stepped);
        parameter_values_[descriptor.index] = descriptor.currentValue;
        parameter_ptrs_.emplace_back(parameter.get());
        parameter_defs_.emplace_back(std::move(parameter));
    }

    if (params_)
        params_->parameterMetadataChangeEvent().notify();
}

bool PluginInstanceWebCLAP::getCachedParameterValue(uint32_t index, double* plainValue) const {
    if (!plainValue)
        return false;
    std::lock_guard<std::mutex> lock(parameter_mutex_);
    auto it = parameter_values_.find(index);
    if (it == parameter_values_.end())
        return false;
    *plainValue = it->second;
    return true;
}

void PluginInstanceWebCLAP::setCachedParameterValue(uint32_t index, double plainValue) {
    std::lock_guard<std::mutex> lock(parameter_mutex_);
    parameter_values_[index] = plainValue;
}

std::string PluginInstanceWebCLAP::buildParameterValueString(uint32_t index, double plainValue) const {
    std::lock_guard<std::mutex> lock(parameter_mutex_);
    for (auto* parameter : parameter_ptrs_) {
        if (parameter && parameter->index() == index) {
            if (parameter->discrete())
                return std::to_string(static_cast<int32_t>(plainValue));
            break;
        }
    }
    return std::to_string(plainValue);
}

StatusCode PluginInstanceWebCLAP::configure(ConfigurationRequest& cfg) {
    // {"type":"wclap-configure","slot":<n>,"sampleRate":<n>,"bufferSize":<n>}
    std::ostringstream json;
    json << "{\"type\":\"wclap-configure\""
         << ",\"slot\":"       << slot_
         << ",\"sampleRate\":" << cfg.sampleRate
         << ",\"bufferSize\":" << cfg.bufferSizeInSamples
         << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
    return StatusCode::OK;
}

StatusCode PluginInstanceWebCLAP::startProcessing() {
    std::ostringstream json;
    json << "{\"type\":\"wclap-start\",\"slot\":" << slot_ << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
    return StatusCode::OK;
}

StatusCode PluginInstanceWebCLAP::stopProcessing() {
    std::ostringstream json;
    json << "{\"type\":\"wclap-stop\",\"slot\":" << slot_ << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
    return StatusCode::OK;
}

void PluginInstanceWebCLAP::attachToTrackGraph(int32_t trackIndex, bool isMasterTrack, uint32_t order) {
    std::ostringstream json;
    json << "{\"type\":\"wclap-graph-add-node\""
         << ",\"slot\":" << slot_
         << ",\"trackIndex\":" << trackIndex
         << ",\"isMaster\":" << (isMasterTrack ? "true" : "false")
         << ",\"order\":" << order
         << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
}

StatusCode PluginInstanceWebCLAP::process(AudioProcessContext& ctx) {
    if (auto json = buildWclapBatchUmpJson(slot_, ctx.eventIn()); !json.empty())
        uapmd_post_to_webclap_worklet_json(json.c_str());
    ctx.copyInputsToOutputs();
    return StatusCode::OK;
}

PluginParameterSupport* PluginInstanceWebCLAP::parameters() {
    return (params_ ? params_ : params_ = std::make_unique<ParamSupportWebCLAP>(this)).get();
}

} // namespace remidy

#endif // __EMSCRIPTEN__
