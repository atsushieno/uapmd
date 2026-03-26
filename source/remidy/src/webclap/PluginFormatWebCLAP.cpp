#ifdef __EMSCRIPTEN__

#include "PluginFormatWebCLAP.hpp"

#include <choc/text/choc_JSON.h>
#include <emscripten.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string_view>
#include <umppi/umppi.hpp>

// Forward declarations: defined in WebAudioWorkletIODevice.cpp as EM_JS functions.
extern "C" void uapmd_ensure_webclap_bridge();
extern "C" void uapmd_post_to_webclap_worklet_rpc(const char* method, const char* argsJson);
extern "C" void uapmd_webclap_create_ui_rpc(uint32_t slot, uint32_t width, uint32_t height);
extern "C" void uapmd_webclap_set_ui_size_rpc(uint32_t slot, uint32_t width, uint32_t height);
extern "C" void uapmd_webclap_load_plugin_async(const char* json);
extern "C" void uapmd_webclap_request_state_rpc(uint32_t reqId, uint32_t slot, uint32_t stateContextType);
extern "C" void uapmd_webclap_load_state_rpc(uint32_t reqId, uint32_t slot, uint32_t stateContextType, const uint8_t* data, size_t size);

EM_JS(void, uapmd_webclap_bind_ui_slot, (uint32_t slot, const char* container_id), {
    Module._uapmdEnsureWebclapBridge().bindUiSlot(slot, UTF8ToString(container_id));
});

EM_JS(void, uapmd_webclap_unbind_ui_slot, (uint32_t slot), {
    Module._uapmdEnsureWebclapBridge().unbindUiSlot(slot);
});

EM_JS(char*, uapmd_webclap_format_parameter_value, (uint32_t slot, uint32_t index, double value), {
    var text = Module._uapmdEnsureWebclapBridge().formatParameterValue(slot, index, value);
    return stringToNewUTF8(text || "");
});

namespace remidy {

struct KnownBundle {
    const char* url;
};

static constexpr KnownBundle kKnownBundles[] = {
    {
        "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/"
            "basics.wclap.tar.gz",
    },
    {
        "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/"
            "example-plugins.wclap.tar.gz",
    },
};

struct WebClapUiMessage {
    bool hasUi{};
    bool canResize{};
    uint32_t width{800};
    uint32_t height{600};
    std::string uri;
};

// ── Shared runtime state for extern "C" callback ──────────────────────────────

struct WebClapGlobalState {
    std::atomic<uint32_t> next_slot{0};
    std::atomic<uint32_t> next_req_id{1};
    std::mutex pending_mutex;
    std::unordered_map<uint32_t, PluginFormatWebCLAPImpl::PendingRequest> pending_requests;
    std::unordered_map<uint32_t, PluginFormatWebCLAPImpl::PendingScanRequest> pending_scan_requests;
    std::unordered_map<uint32_t, PluginFormatWebCLAPImpl::PendingStateRequest> pending_state_requests;
    std::unordered_map<uint32_t, PluginFormatWebCLAPImpl::PendingStateLoadRequest> pending_state_load_requests;
    std::mutex instances_mutex;
    std::unordered_map<uint32_t, PluginInstanceWebCLAP*> instances_by_slot;
    std::unordered_map<uint32_t, std::vector<WebClapParamDescriptor>> pending_parameters_by_slot;
    std::unordered_map<uint32_t, WebClapUiMessage> pending_ui_by_slot;
    std::unordered_map<uint32_t, WebClapCapabilities> pending_capabilities_by_slot;
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

// Called from the bridge in WebAudioWorkletIODevice.cpp when browser-side
// WebCLAP operations report completion or metadata back to remidy. The exported
// C symbol name is kept stable because the bridge calls it directly.

extern "C" EMSCRIPTEN_KEEPALIVE
void uapmd_webclap_on_worklet_message(const char* json) {
    PluginFormatWebCLAPImpl impl;
    impl.onBridgeMessage(json);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void uapmd_webclap_on_worklet_state_response(uint32_t reqId, const uint8_t* data, size_t size, const char* error) {
    PluginFormatWebCLAPImpl impl;
    impl.onBridgeStateResponse(reqId, data, size, error);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void uapmd_webclap_on_worklet_state_load_complete(uint32_t reqId, const char* error) {
    PluginFormatWebCLAPImpl impl;
    impl.onBridgeStateLoadComplete(reqId, error);
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
        descriptor.automatablePerKey = item["automatablePerKey"].getWithDefault<bool>(false);
        descriptor.automatablePerChannel = item["automatablePerChannel"].getWithDefault<bool>(false);
        descriptor.automatablePerPort = item["automatablePerPort"].getWithDefault<bool>(false);
        descriptor.modulatablePerKey = item["modulatablePerKey"].getWithDefault<bool>(false);
        descriptor.modulatablePerChannel = item["modulatablePerChannel"].getWithDefault<bool>(false);
        descriptor.modulatablePerPort = item["modulatablePerPort"].getWithDefault<bool>(false);
        descriptor.modulatablePerNoteId = item["modulatablePerNoteId"].getWithDefault<bool>(false);
        descriptors.emplace_back(std::move(descriptor));
    }
    return descriptors;
}

static std::vector<PluginCatalogEntry> parsePluginCatalogEntries(const choc::value::ValueView& value,
                                                                 const std::filesystem::path& bundlePath) {
    std::vector<PluginCatalogEntry> entries;
    if (!value.isArray())
        return entries;
    entries.reserve(value.size());
    for (uint32_t i = 0, size = value.size(); i < size; ++i) {
        auto item = value[i];
        PluginCatalogEntry entry{};
        static std::string formatName{"WebCLAP"};
        entry.format(formatName);
        std::string pluginId = item["id"].toString();
        entry.pluginId(pluginId);
        entry.displayName(item["name"].toString());
        entry.vendorName(item["vendor"].toString());
        entry.productUrl(item["url"].toString());
        entry.bundlePath(bundlePath);
        entries.emplace_back(std::move(entry));
    }
    return entries;
}

static WebClapCapabilities parseCapabilities(const choc::value::ValueView& value) {
    WebClapCapabilities capabilities;
    capabilities.hasEventInputs = value["hasEventInputs"].getWithDefault<bool>(false);
    capabilities.hasEventOutputs = value["hasEventOutputs"].getWithDefault<bool>(false);
    capabilities.hasState = value["hasState"].getWithDefault<bool>(false);
    capabilities.hasPresetLoad = value["hasPresetLoad"].getWithDefault<bool>(false);
    return capabilities;
}

static WebClapUiMessage parseUiMessage(const choc::value::ValueView& value) {
    WebClapUiMessage ui;
    ui.hasUi = value["hasUi"].getWithDefault<bool>(!value["uri"].toString().empty());
    ui.canResize = value["canResize"].getWithDefault<bool>(false);
    ui.width = static_cast<uint32_t>(value["width"].getWithDefault<int32_t>(800));
    ui.height = static_cast<uint32_t>(value["height"].getWithDefault<int32_t>(600));
    ui.uri = value["uri"].toString();
    return ui;
}

static std::vector<std::pair<uint32_t, double>> parseParameterValueUpdates(const choc::value::ValueView& value) {
    std::vector<std::pair<uint32_t, double>> updates;
    if (!value.isArray())
        return updates;
    auto size = value.size();
    updates.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        auto item = value[i];
        if (!item.isObject())
            continue;
        auto indexValue = item["index"].getWithDefault<int32_t>(-1);
        if (indexValue < 0)
            continue;
        auto plainValue = item["value"].getWithDefault<double>(0.0);
        updates.emplace_back(static_cast<uint32_t>(indexValue), plainValue);
    }
    return updates;
}

static void postWclapRpc(const char* method, const std::string& argsJson) {
    uapmd_ensure_webclap_bridge();
    uapmd_post_to_webclap_worklet_rpc(method, argsJson.c_str());
}

static std::string buildWclapBatchUmpArgsJson(EventSequence& eventIn) {
    auto* bytes = static_cast<const uint8_t*>(eventIn.getMessages());
    const size_t bytes_available = eventIn.position();
    if (!bytes || bytes_available == 0)
        return {};

    std::ostringstream payload;
    payload << "[";

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
    return payload.str();
}

std::vector<PluginParameter*>& PluginInstanceWebCLAP::ParamSupportWebCLAP::parameters() {
    return owner_->parameterPointers();
}

std::vector<PluginParameter*>& PluginInstanceWebCLAP::ParamSupportWebCLAP::perNoteControllers(
    PerNoteControllerContextTypes types, PerNoteControllerContext)
{
    auto& params = owner_->parameterPointers();
    auto& filtered = owner_->perNoteParameterPointers();
    filtered.clear();
    if (types == PER_NOTE_CONTROLLER_NONE)
        return params;
    filtered.reserve(params.size());
    for (uint32_t index = 0; index < params.size(); ++index) {
        if (owner_->parameterSupportsContext(index, types))
            filtered.emplace_back(params[index]);
    }
    return filtered;
}

StatusCode PluginInstanceWebCLAP::ParamSupportWebCLAP::setParameter(
    uint32_t index, double plainValue, uint64_t)
{
    owner_->setCachedParameterValue(index, plainValue);
    std::ostringstream args;
    args << "[" << owner_->slot() << "," << index << "," << plainValue << "]";
    postWclapRpc("setParameter", args.str());
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
    PerNoteControllerContext context, uint32_t index, double plainValue, uint64_t)
{
    owner_->setCachedParameterValue(index, plainValue);
    std::ostringstream args;
    args << "[" << owner_->slot()
         << "," << index
         << "," << plainValue
         << ",true"
         << "," << context.group
         << "," << context.channel
         << "," << context.note
         << "]";
    postWclapRpc("setParameter", args.str());

    PerNoteControllerContextTypes contextType = PER_NOTE_CONTROLLER_NONE;
    if (context.group != 0)
        contextType = static_cast<PerNoteControllerContextTypes>(contextType | PER_NOTE_CONTROLLER_PER_GROUP);
    if (context.channel != 0)
        contextType = static_cast<PerNoteControllerContextTypes>(contextType | PER_NOTE_CONTROLLER_PER_CHANNEL);
    if (context.note != 0)
        contextType = static_cast<PerNoteControllerContextTypes>(contextType | PER_NOTE_CONTROLLER_PER_NOTE);

    uint32_t contextValue = context.note;
    perNoteControllerChangeEvent().notify(contextType, contextValue, index, plainValue);
    return StatusCode::OK;
}

StatusCode PluginInstanceWebCLAP::ParamSupportWebCLAP::getPerNoteController(
    PerNoteControllerContext, uint32_t index, double* value)
{
    (void) index;
    (void) value;
    return StatusCode::NOT_IMPLEMENTED;
}

std::string PluginInstanceWebCLAP::ParamSupportWebCLAP::valueToString(uint32_t index, double v) {
    return owner_->buildParameterValueString(index, v);
}

std::string PluginInstanceWebCLAP::ParamSupportWebCLAP::valueToStringPerNote(
    PerNoteControllerContext, uint32_t index, double v)
{
    return valueToString(index, v);
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
    auto capsIt = state.pending_capabilities_by_slot.find(instance->slot());
    if (capsIt != state.pending_capabilities_by_slot.end()) {
        instance->updateCapabilities(capsIt->second);
        state.pending_capabilities_by_slot.erase(capsIt);
    }
    auto uiIt = state.pending_ui_by_slot.find(instance->slot());
    if (uiIt != state.pending_ui_by_slot.end()) {
        instance->updateUiInfo(uiIt->second.hasUi, uiIt->second.canResize, uiIt->second.width, uiIt->second.height);
        state.pending_ui_by_slot.erase(uiIt);
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
    std::string clapId = info->pluginId();
    auto bundlePath = info->bundlePath();
    std::string bundleUrl = bundlePath.empty() ? info->pluginId() : bundlePath.string();

    {
        std::lock_guard<std::mutex> lock(state.pending_mutex);
        state.pending_requests.emplace(req_id, PendingRequest{info, slot, std::move(callback)});
    }

    // Ask the browser-side bridge to fetch and inspect the bundle on the main
    // thread, then instantiate it in the worklet runtime.
    std::ostringstream json;
    json << "{\"type\":\"webclap-instantiate\""
         << ",\"reqId\":"  << req_id
         << ",\"slot\":"   << slot
         << ",\"url\":\""  << bundleUrl << "\"";
    if (!clapId.empty() && clapId != bundleUrl)
        json << ",\"pluginId\":\"" << clapId << "\"";
    json
         << "}";
    auto requestJson = json.str();
    EventLoop::runTaskOnMainThread([requestJson]() {
        uapmd_ensure_webclap_bridge();
        uapmd_webclap_load_plugin_async(requestJson.c_str());
    });
}

void PluginFormatWebCLAPImpl::startBundleScan(const std::filesystem::path& bundlePath,
                                              std::function<void(PluginCatalogEntry entry)> pluginFound,
                                              PluginScanCompletedCallback scanCompleted) {
    auto& state = webclapGlobalState();
    const uint32_t req_id = state.next_req_id.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(state.pending_mutex);
        state.pending_scan_requests.emplace(req_id, PendingScanRequest{bundlePath, std::move(pluginFound), std::move(scanCompleted)});
    }

    std::ostringstream json;
    json << "{\"type\":\"webclap-scan\""
         << ",\"reqId\":" << req_id
         << ",\"url\":\"" << bundlePath.string() << "\""
         << "}";
    auto requestJson = json.str();
    EventLoop::runTaskOnMainThread([requestJson]() {
        uapmd_ensure_webclap_bridge();
        uapmd_webclap_load_plugin_async(requestJson.c_str());
    });
}

uint32_t PluginFormatWebCLAPImpl::reserveRequestId() {
    auto& state = webclapGlobalState();
    return state.next_req_id.fetch_add(1, std::memory_order_relaxed);
}

void PluginFormatWebCLAPImpl::registerPendingStateRequest(
        uint32_t reqId,
        uint32_t slot,
        std::function<void(std::vector<uint8_t> state, std::string error)> callback) {
    auto& state = webclapGlobalState();
    std::lock_guard<std::mutex> lock(state.pending_mutex);
    state.pending_state_requests.emplace(reqId, PendingStateRequest{slot, std::move(callback)});
}

void PluginFormatWebCLAPImpl::registerPendingStateLoadRequest(
        uint32_t reqId,
        uint32_t slot,
        std::function<void(std::string error)> callback) {
    auto& state = webclapGlobalState();
    std::lock_guard<std::mutex> lock(state.pending_mutex);
    state.pending_state_load_requests.emplace(reqId, PendingStateLoadRequest{slot, std::move(callback)});
}

void PluginFormatWebCLAPImpl::onBridgeMessage(const char* json_cstr) {
    const auto message = choc::json::parse(json_cstr);
    const auto type = message["type"].toString();

    if (type == "webclap-instance-created") {
        const uint32_t req_id = static_cast<uint32_t>(message["reqId"].getWithDefault<int32_t>(0));
        const uint32_t slot   = static_cast<uint32_t>(message["slot"].getWithDefault<int32_t>(0));
        auto& state = webclapGlobalState();

        PendingRequest req;
        {
            std::lock_guard<std::mutex> lock(state.pending_mutex);
            auto it = state.pending_requests.find(req_id);
            if (it == state.pending_requests.end()) {
                std::cerr << "[WebCLAP] plugin-ready: unknown reqId "
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

    } else if (type == "webclap-instance-create-failed") {
        const uint32_t req_id = static_cast<uint32_t>(message["reqId"].getWithDefault<int32_t>(0));
        std::string error = message["error"].toString();
        auto& state = webclapGlobalState();

        PendingRequest req;
        {
            std::lock_guard<std::mutex> lock(state.pending_mutex);
            auto it = state.pending_requests.find(req_id);
            if (it == state.pending_requests.end()) {
                std::cerr << "[WebCLAP] plugin-error: unknown reqId "
                          << req_id << std::endl;
                return;
            }
            req = std::move(it->second);
            state.pending_requests.erase(it);
        }
        req.callback(nullptr, error);
    } else if (type == "webclap-scan-complete") {
        const uint32_t req_id = static_cast<uint32_t>(message["reqId"].getWithDefault<int32_t>(0));
        auto& state = webclapGlobalState();

        PendingScanRequest req;
        {
            std::lock_guard<std::mutex> lock(state.pending_mutex);
            auto it = state.pending_scan_requests.find(req_id);
            if (it == state.pending_scan_requests.end()) {
                std::cerr << "[WebCLAP] scan-result: unknown reqId "
                          << req_id << std::endl;
                return;
            }
            req = std::move(it->second);
            state.pending_scan_requests.erase(it);
        }

        auto entries = parsePluginCatalogEntries(message["plugins"], req.bundlePath);
        for (auto& entry : entries)
            if (req.pluginFound)
                req.pluginFound(std::move(entry));
        if (req.scanCompleted)
            req.scanCompleted("");
    } else if (type == "webclap-scan-failed") {
        const uint32_t req_id = static_cast<uint32_t>(message["reqId"].getWithDefault<int32_t>(0));
        auto& state = webclapGlobalState();

        PendingScanRequest req;
        {
            std::lock_guard<std::mutex> lock(state.pending_mutex);
            auto it = state.pending_scan_requests.find(req_id);
            if (it == state.pending_scan_requests.end()) {
                std::cerr << "[WebCLAP] scan-error: unknown reqId "
                          << req_id << std::endl;
                return;
            }
            req = std::move(it->second);
            state.pending_scan_requests.erase(it);
        }

        if (req.scanCompleted)
            req.scanCompleted(message["error"].toString());
    } else if (type == "webclap-parameter-descriptors") {
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
    } else if (type == "webclap-ui-descriptor" || type == "webclap-ui-opened" || type == "webclap-ui-resized") {
        const uint32_t slot = static_cast<uint32_t>(message["slot"].getWithDefault<int32_t>(0));
        auto ui = parseUiMessage(message);
        PluginInstanceWebCLAP* instance = nullptr;
        auto& state = webclapGlobalState();
        {
            std::lock_guard<std::mutex> lock(state.instances_mutex);
            auto it = state.instances_by_slot.find(slot);
            if (it != state.instances_by_slot.end())
                instance = it->second;
            else
                state.pending_ui_by_slot[slot] = ui;
        }
        if (instance) {
            instance->updateUiInfo(ui.hasUi, ui.canResize, ui.width, ui.height);
            if (type == "webclap-ui-resized")
                instance->notifyUiResizeRequest(ui.canResize, ui.width, ui.height);
        }
    } else if (type == "webclap-parameter-values-updated") {
        const uint32_t slot = static_cast<uint32_t>(message["slot"].getWithDefault<int32_t>(0));
        auto updates = parseParameterValueUpdates(message["updates"]);
        PluginInstanceWebCLAP* instance = nullptr;
        auto& state = webclapGlobalState();
        {
            std::lock_guard<std::mutex> lock(state.instances_mutex);
            auto it = state.instances_by_slot.find(slot);
            if (it != state.instances_by_slot.end())
                instance = it->second;
        }
        if (instance) {
            for (const auto& update : updates)
                instance->applyParameterValueUpdate(update.first, update.second);
        }
    } else if (type == "webclap-capabilities") {
        const uint32_t slot = static_cast<uint32_t>(message["slot"].getWithDefault<int32_t>(0));
        auto capabilities = parseCapabilities(message);
        PluginInstanceWebCLAP* instance = nullptr;
        auto& state = webclapGlobalState();
        {
            std::lock_guard<std::mutex> lock(state.instances_mutex);
            auto it = state.instances_by_slot.find(slot);
            if (it != state.instances_by_slot.end())
                instance = it->second;
            else
                state.pending_capabilities_by_slot[slot] = capabilities;
        }
        if (instance)
            instance->updateCapabilities(capabilities);
    } else if (type == "webclap-runtime-error") {
        std::cerr << "[WebCLAP] runtime error: " << message["error"].toString() << std::endl;
    }
    // All other message types (wclap-host-ready, etc.) are silently ignored here.
}

void PluginFormatWebCLAPImpl::onBridgeStateResponse(uint32_t reqId, const uint8_t* data, size_t size, const char* error) {
    auto& state = webclapGlobalState();
    PendingStateRequest request{};
    {
        std::lock_guard<std::mutex> lock(state.pending_mutex);
        auto it = state.pending_state_requests.find(reqId);
        if (it == state.pending_state_requests.end()) {
            std::cerr << "[WebCLAP] state-request completion for unknown reqId "
                      << reqId << std::endl;
            return;
        }
        request = std::move(it->second);
        state.pending_state_requests.erase(it);
    }

    std::vector<uint8_t> response{};
    if (data && size > 0) {
        response.resize(size);
        memcpy(response.data(), data, size);
    }
    request.callback(std::move(response), error ? error : "");
}

void PluginFormatWebCLAPImpl::onBridgeStateLoadComplete(uint32_t reqId, const char* error) {
    auto& state = webclapGlobalState();
    PendingStateLoadRequest request{};
    {
        std::lock_guard<std::mutex> lock(state.pending_mutex);
        auto it = state.pending_state_load_requests.find(reqId);
        if (it == state.pending_state_load_requests.end()) {
            std::cerr << "[WebCLAP] state-load completion for unknown reqId "
                      << reqId << std::endl;
            return;
        }
        request = std::move(it->second);
        state.pending_state_load_requests.erase(it);
    }

    request.callback(error ? error : "");
}

// ── PluginScanningWebCLAP ─────────────────────────────────────────────────────

std::vector<PluginCatalogEntry>
PluginScanningWebCLAP::getAllFastScannablePlugins() {
    return {};
}

std::vector<std::filesystem::path>& PluginScanningWebCLAP::getDefaultSearchPaths() {
    static std::vector<std::filesystem::path> paths{};
    return paths;
}

void PluginScanningWebCLAP::scanBundle(const std::filesystem::path& bundlePath,
                                       bool /*requireFastScanning*/,
                                       double /*timeoutSeconds*/,
                                       std::function<void(PluginCatalogEntry entry)> pluginFound,
                                       PluginScanCompletedCallback scanCompleted) {
    owner_->startBundleScan(bundlePath, std::move(pluginFound), std::move(scanCompleted));
}

std::vector<std::filesystem::path> PluginScanningWebCLAP::enumerateCandidateBundles(bool /*requireFastScanning*/) {
    std::vector<std::filesystem::path> bundles;
    bundles.reserve(std::size(kKnownBundles));
    for (const auto& bundle : kKnownBundles)
        bundles.emplace_back(bundle.url);
    return bundles;
}

void PluginScanningWebCLAP::startSlowPluginScan(std::function<void(PluginCatalogEntry entry)> pluginFound,
                                                PluginScanCompletedCallback scanCompleted) {
    auto bundles = enumerateCandidateBundles(false);
    auto bundleList = std::make_shared<std::vector<std::filesystem::path>>(std::move(bundles));
    auto index = std::make_shared<size_t>(0);
    auto continuation = std::make_shared<std::function<void()>>();
    *continuation = [this, bundleList, index, pluginFound, scanCompleted, continuation]() mutable {
        if (*index >= bundleList->size()) {
            if (scanCompleted)
                scanCompleted("");
            return;
        }
        auto bundlePath = (*bundleList)[(*index)++];
        scanBundle(bundlePath, false, 0.0, pluginFound,
                   [scanCompleted, continuation](std::string error) mutable {
                       if (!error.empty()) {
                           if (scanCompleted)
                               scanCompleted(std::move(error));
                           return;
                       }
                       (*continuation)();
                   });
    };
    (*continuation)();
}

// ── PluginInstanceWebCLAP ─────────────────────────────────────────────────────

PluginInstanceWebCLAP::PluginInstanceWebCLAP(PluginCatalogEntry* entry, uint32_t slot)
    : PluginInstance(entry), slot_(slot)
{}

PluginInstanceWebCLAP::~PluginInstanceWebCLAP() {
    std::ostringstream args;
    args << "[" << slot_ << "]";
    postWclapRpc("unload", args.str());
    unregisterWebClapInstance(slot_);
}

void PluginInstanceWebCLAP::updateParameters(const std::vector<WebClapParamDescriptor>& descriptors) {
    std::lock_guard<std::mutex> lock(parameter_mutex_);

    parameter_descriptors_ = descriptors;
    parameter_defs_.clear();
    parameter_ptrs_.clear();
    per_note_parameter_ptrs_.clear();
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

void PluginInstanceWebCLAP::updateCapabilities(const WebClapCapabilities& capabilities) {
    has_event_inputs_ = capabilities.hasEventInputs;
    has_event_outputs_ = capabilities.hasEventOutputs;
    has_state_support_ = capabilities.hasState;
    has_preset_load_support_ = capabilities.hasPresetLoad;
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

void PluginInstanceWebCLAP::applyParameterValueUpdate(uint32_t index, double plainValue) {
    setCachedParameterValue(index, plainValue);
    if (params_)
        params_->parameterChangeEvent().notify(index, plainValue);
}

bool PluginInstanceWebCLAP::hasUiSupport() const {
    return has_ui_;
}

void PluginInstanceWebCLAP::updateUiInfo(bool hasUi, bool canResize, uint32_t width, uint32_t height) {
    has_ui_ = hasUi;
    ui_can_resize_ = canResize;
    if (width > 0)
        ui_width_ = width;
    if (height > 0)
        ui_height_ = height;
    if (ui_)
        ui_->updateUiState(ui_can_resize_, ui_width_, ui_height_);
}

void PluginInstanceWebCLAP::notifyUiResizeRequest(bool canResize, uint32_t width, uint32_t height) {
    updateUiInfo(true, canResize, width, height);
    if (ui_)
        ui_->updateUiState(ui_can_resize_, ui_width_, ui_height_);
}

bool PluginInstanceWebCLAP::getUiSize(uint32_t& width, uint32_t& height) const {
    if (!has_ui_)
        return false;
    width = ui_width_;
    height = ui_height_;
    return true;
}

bool PluginInstanceWebCLAP::canUiResize() const {
    return has_ui_ && ui_can_resize_;
}

bool PluginInstanceWebCLAP::parameterSupportsContext(uint32_t index, PerNoteControllerContextTypes types) const {
    if (index >= parameter_descriptors_.size())
        return false;
    const auto& descriptor = parameter_descriptors_[index];
    if (types & PER_NOTE_CONTROLLER_PER_GROUP) {
        if (!descriptor.automatablePerPort && !descriptor.modulatablePerPort)
            return false;
    }
    if (types & PER_NOTE_CONTROLLER_PER_CHANNEL) {
        if (!descriptor.automatablePerChannel && !descriptor.modulatablePerChannel)
            return false;
    }
    if (types & PER_NOTE_CONTROLLER_PER_NOTE) {
        if (!descriptor.automatablePerKey && !descriptor.modulatablePerKey &&
            !descriptor.modulatablePerNoteId)
            return false;
    }
    return true;
}

std::string PluginInstanceWebCLAP::buildParameterValueString(uint32_t index, double plainValue) const {
    char* formatted = uapmd_webclap_format_parameter_value(slot_, index, plainValue);
    if (formatted) {
        std::string text{formatted};
        std::free(formatted);
        if (!text.empty())
            return text;
    }

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

bool PluginInstanceWebCLAP::BusesWebCLAP::hasEventInputs() {
    return owner_ && owner_->hasEventInputs();
}

bool PluginInstanceWebCLAP::BusesWebCLAP::hasEventOutputs() {
    return owner_ && owner_->hasEventOutputs();
}

std::vector<uint8_t> PluginInstanceWebCLAP::StateSupportWebCLAP::getState(StateContextType, bool) {
    if (owner_ && owner_->hasStateSupport())
        std::cerr << "[WebCLAP] state save is not implemented for worklet-owned instances\n";
    return {};
}

void PluginInstanceWebCLAP::StateSupportWebCLAP::setState(std::vector<uint8_t>&, StateContextType, bool) {
    if (owner_ && owner_->hasStateSupport())
        std::cerr << "[WebCLAP] state load is not implemented for worklet-owned instances\n";
}

void PluginInstanceWebCLAP::StateSupportWebCLAP::requestState(
        StateContextType stateContextType,
        bool includeUiState,
        void* callbackContext,
        std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)> receiver) {
    queue_.enqueueRequest(callbackContext, std::move(receiver),
                          [this, stateContextType, includeUiState](std::function<bool()> isCancelled,
                                                                    std::function<void(std::vector<uint8_t> state, std::string error)> finish) mutable {
                              EventLoop::enqueueTaskOnMainThread([this, isCancelled, stateContextType, includeUiState, finish = std::move(finish)]() mutable {
                                  if (isCancelled()) {
                                      finish({}, "instance destroyed");
                                      return;
                                  }
                                  if (!owner_ || !owner_->hasStateSupport()) {
                                      finish({}, "State is not supported");
                                      return;
                                  }

                                  auto reqId = PluginFormatWebCLAPImpl{}.reserveRequestId();
                                  PluginFormatWebCLAPImpl{}.registerPendingStateRequest(
                                          reqId,
                                          owner_->slot(),
                                          [finish = std::move(finish)](std::vector<uint8_t> state, std::string error) mutable {
                                              finish(std::move(state), std::move(error));
                                          });

                                  uapmd_ensure_webclap_bridge();
                                  uapmd_webclap_request_state_rpc(reqId,
                                                                 owner_->slot(),
                                                                 static_cast<uint32_t>(stateContextType));
                              });
                          });
}

void PluginInstanceWebCLAP::StateSupportWebCLAP::loadState(
        std::vector<uint8_t> state,
        StateContextType stateContextType,
        bool includeUiState,
        void* callbackContext,
        std::function<void(std::string error, void* callbackContext)> completed) {
    queue_.enqueueLoad(callbackContext, std::move(completed),
                       [this, state = std::move(state), stateContextType, includeUiState](std::function<bool()> isCancelled,
                                                                                           std::function<void(std::string error)> finish) mutable {
                           EventLoop::enqueueTaskOnMainThread([this, isCancelled, state = std::move(state), stateContextType, includeUiState, finish = std::move(finish)]() mutable {
                               if (isCancelled()) {
                                   finish("instance destroyed");
                                   return;
                               }
                               if (!owner_ || !owner_->hasStateSupport()) {
                                   finish("State is not supported");
                                   return;
                               }

                               auto reqId = PluginFormatWebCLAPImpl{}.reserveRequestId();
                               PluginFormatWebCLAPImpl{}.registerPendingStateLoadRequest(
                                       reqId,
                                       owner_->slot(),
                                       [finish = std::move(finish)](std::string error) mutable {
                                           finish(std::move(error));
                                       });
                               uapmd_ensure_webclap_bridge();
                               uapmd_webclap_load_state_rpc(reqId,
                                                            owner_->slot(),
                                                            static_cast<uint32_t>(stateContextType),
                                                            state.data(),
                                                            state.size());
                           });
                       });
}

int32_t PluginInstanceWebCLAP::PresetsSupportWebCLAP::getPresetIndexForId(std::string&) {
    return -1;
}

int32_t PluginInstanceWebCLAP::PresetsSupportWebCLAP::getPresetCount() {
    return 0;
}

PresetInfo PluginInstanceWebCLAP::PresetsSupportWebCLAP::getPresetInfo(int32_t) {
    return {"", "", 0, 0};
}

void PluginInstanceWebCLAP::PresetsSupportWebCLAP::loadPreset(int32_t) {
    if (owner_ && owner_->hasPresetLoadSupport())
        std::cerr << "[WebCLAP] preset loading is not implemented for worklet-owned instances\n";
}

StatusCode PluginInstanceWebCLAP::configure(ConfigurationRequest& cfg) {
    std::ostringstream args;
    args << "[" << slot_ << "," << cfg.sampleRate << "," << cfg.bufferSizeInSamples << "]";
    postWclapRpc("configure", args.str());
    return StatusCode::OK;
}

StatusCode PluginInstanceWebCLAP::startProcessing() {
    std::ostringstream args;
    args << "[" << slot_ << "]";
    postWclapRpc("start", args.str());
    return StatusCode::OK;
}

StatusCode PluginInstanceWebCLAP::stopProcessing() {
    std::ostringstream args;
    args << "[" << slot_ << "]";
    postWclapRpc("stop", args.str());
    return StatusCode::OK;
}

void PluginInstanceWebCLAP::attachToTrackGraph(int32_t trackIndex, bool isMasterTrack, uint32_t order) {
    std::ostringstream args;
    args << "[" << slot_
         << "," << (isMasterTrack ? "true" : "false")
         << "," << trackIndex
         << "," << order
         << "]";
    postWclapRpc("graphAddNode", args.str());
}

StatusCode PluginInstanceWebCLAP::process(AudioProcessContext& ctx) {
    if (auto args = buildWclapBatchUmpArgsJson(ctx.eventIn()); !args.empty()) {
        std::ostringstream rpcArgs;
        rpcArgs << "[" << slot_ << "," << args << "]";
        postWclapRpc("sendUmpBatch", rpcArgs.str());
    }
    ctx.copyInputsToOutputs();
    return StatusCode::OK;
}

PluginParameterSupport* PluginInstanceWebCLAP::parameters() {
    return (params_ ? params_ : params_ = std::make_unique<ParamSupportWebCLAP>(this)).get();
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::hasUI() {
    return owner_ && owner_->hasUiSupport();
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::create(
    bool, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler)
{
    if (created_ || !owner_ || !parentHandle || !owner_->hasUiSupport())
        return false;
    container_id_ = static_cast<const char*>(parentHandle);
    if (container_id_.empty())
        return false;
    owner_->getUiSize(width_, height_);
    can_resize_ = owner_->canUiResize();
    resize_handler_ = std::move(resizeHandler);
    uapmd_webclap_bind_ui_slot(owner_->slot(), container_id_.c_str());
    uapmd_ensure_webclap_bridge();
    uapmd_webclap_create_ui_rpc(owner_->slot(), width_, height_);
    created_ = true;
    return true;
}

void PluginInstanceWebCLAP::UISupportWebCLAP::destroy() {
    if (!owner_ || !created_)
        return;
    std::ostringstream args;
    args << "[" << owner_->slot() << "]";
    postWclapRpc("destroyUi", args.str());
    uapmd_webclap_unbind_ui_slot(owner_->slot());
    created_ = false;
    visible_ = false;
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::show() {
    if (!owner_ || !created_)
        return false;
    std::ostringstream args;
    args << "[" << owner_->slot() << "]";
    postWclapRpc("showUi", args.str());
    visible_ = true;
    return true;
}

void PluginInstanceWebCLAP::UISupportWebCLAP::hide() {
    if (!owner_ || !created_)
        return;
    std::ostringstream args;
    args << "[" << owner_->slot() << "]";
    postWclapRpc("hideUi", args.str());
    visible_ = false;
}

void PluginInstanceWebCLAP::UISupportWebCLAP::setWindowTitle(std::string) {
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::canResize() {
    return can_resize_;
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::getSize(uint32_t& width, uint32_t& height) {
    width = width_;
    height = height_;
    return owner_ && owner_->hasUiSupport();
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::setSize(uint32_t width, uint32_t height) {
    if (!owner_ || !created_)
        return false;
    if (width_ == width && height_ == height)
        return true;
    width_ = width;
    height_ = height;
    uapmd_ensure_webclap_bridge();
    uapmd_webclap_set_ui_size_rpc(owner_->slot(), width, height);
    return true;
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::suggestSize(uint32_t& width, uint32_t& height) {
    return getSize(width, height);
}

void PluginInstanceWebCLAP::UISupportWebCLAP::updateUiState(bool canResize, uint32_t width, uint32_t height) {
    const bool size_changed = width_ != width || height_ != height;
    can_resize_ = canResize;
    width_ = width;
    height_ = height;
    if (resize_handler_ && size_changed && width > 0 && height > 0)
        resize_handler_(width, height);
}

} // namespace remidy

#endif // __EMSCRIPTEN__
