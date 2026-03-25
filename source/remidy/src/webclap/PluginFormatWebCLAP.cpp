#ifdef __EMSCRIPTEN__

#include "PluginFormatWebCLAP.hpp"

#include <choc/text/choc_JSON.h>
#include <emscripten.h>
#include <iostream>
#include <sstream>
#include <string_view>
#include <umppi/umppi.hpp>

// Forward declarations: defined in WebAudioWorkletIODevice.cpp as EM_JS functions.
extern "C" void uapmd_post_to_webclap_worklet_json(const char* json);
extern "C" void uapmd_webclap_load_plugin_async(const char* json);

EM_JS(void, uapmd_webclap_bind_ui_slot, (uint32_t slot, const char* container_id), {
    if (!Module._uapmdEnsureWebclapUiManager) {
        Module._uapmdEnsureWebclapUiManager = function() {
            if (Module._uapmdWebclapUi)
                return Module._uapmdWebclapUi;
            const manager = {
                bindings: new Map(),
                bind(slot, containerId) {
                    const binding = { slot, containerId, iframe: null, uri: "", bodyId: containerId };
                    this.bindings.set(slot, binding);
                },
                unbind(slot) {
                    const binding = this.bindings.get(slot);
                    if (binding && binding.blobUrls) {
                        Object.values(binding.blobUrls).forEach(function(url) { URL.revokeObjectURL(url); });
                    }
                    if (binding && binding.iframe)
                        binding.iframe.remove();
                    this.bindings.delete(slot);
                },
                getBody(slot) {
                    const binding = this.bindings.get(slot);
                    if (!binding)
                        return null;
                    return document.getElementById(binding.containerId);
                },
                mimeFor(path) {
                    if (path.endsWith('.html')) return 'text/html';
                    if (path.endsWith('.js') || path.endsWith('.mjs')) return 'text/javascript';
                    if (path.endsWith('.css')) return 'text/css';
                    if (path.endsWith('.svg')) return 'image/svg+xml';
                    if (path.endsWith('.json')) return 'application/json';
                    if (path.endsWith('.wasm')) return 'application/wasm';
                    return 'application/octet-stream';
                },
                findFileKey(files, uri) {
                    if (!files)
                        return null;
                    let normalized = uri;
                    if (normalized.startsWith('file://'))
                        normalized = normalized.slice(7);
                    else if (normalized.startsWith('file:'))
                        normalized = normalized.slice(5);
                    if (files[normalized]) return normalized;
                    if (files['/' + normalized]) return '/' + normalized;
                    for (const key of Object.keys(files)) {
                        if (normalized.endsWith(key) || key.endsWith(normalized))
                            return key;
                    }
                    return null;
                },
                open(slot, uri, files) {
                    const binding = this.bindings.get(slot);
                    const body = this.getBody(slot);
                    if (!binding || !body)
                        return;
                    if (binding.blobUrls) {
                        Object.values(binding.blobUrls).forEach(function(url) { URL.revokeObjectURL(url); });
                        binding.blobUrls = null;
                    }
                    if (binding.iframe)
                        binding.iframe.remove();
                    body.dataset.webclapSlot = String(slot);
                    body.textContent = "";
                    const iframe = document.createElement('iframe');
                    iframe.id = `uapmd-webclap-frame-${slot}`;
                    iframe.dataset.webclapSlot = String(slot);
                    iframe.style.border = '0';
                    iframe.style.width = '100%';
                    iframe.style.height = '100%';
                    iframe.style.background = '#111';
                    body.appendChild(iframe);
                    binding.iframe = iframe;
                    binding.uri = uri;
                    if ((uri.startsWith('file:') || uri.startsWith('/')) && files) {
                        const fileKey = this.findFileKey(files, uri);
                        if (fileKey) {
                            const root = files[fileKey];
                            const blobUrls = {};
                            for (const [path, content] of Object.entries(files)) {
                                const blob = new Blob([content], { type: this.mimeFor(path) });
                                blobUrls[path] = URL.createObjectURL(blob);
                            }
                            if (fileKey.endsWith('.html')) {
                                let html = new TextDecoder('utf-8').decode(root);
                                for (const [path, blobUrl] of Object.entries(blobUrls)) {
                                    const basename = path.split('/').pop();
                                    html = html.split(path).join(blobUrl);
                                    if (basename)
                                        html = html.split(basename).join(blobUrl);
                                }
                                iframe.srcdoc = html;
                            } else {
                                iframe.src = blobUrls[fileKey];
                            }
                            binding.blobUrls = blobUrls;
                        } else {
                            iframe.src = uri;
                        }
                    } else {
                        iframe.src = uri;
                    }
                },
                postToFrame(slot, payload) {
                    const binding = this.bindings.get(slot);
                    if (!binding || !binding.iframe || !binding.iframe.contentWindow)
                        return;
                    if (payload instanceof ArrayBuffer) {
                        binding.iframe.contentWindow.postMessage(payload, '*', [payload]);
                        return;
                    }
                    if (ArrayBuffer.isView(payload)) {
                        binding.iframe.contentWindow.postMessage(payload.buffer, '*', [payload.buffer]);
                        return;
                    }
                    let value = payload;
                    if (typeof payload === 'string') {
                        try { value = JSON.parse(payload); } catch (_) {}
                    }
                    binding.iframe.contentWindow.postMessage(value, '*');
                },
            };
            window.addEventListener('message', function(event) {
                for (const [slot, binding] of manager.bindings.entries()) {
                    if (!binding.iframe || event.source !== binding.iframe.contentWindow)
                        continue;
                    const node = Module._wclapWorkletNode;
                    if (node) {
                        const payload = event.data;
                        if (payload instanceof ArrayBuffer)
                            node.port.postMessage({ type: 'wclap-ui-from-frame', slot, payload }, [payload]);
                        else
                            node.port.postMessage({ type: 'wclap-ui-from-frame', slot, payload });
                    }
                    break;
                }
            });
            Module._uapmdWebclapUi = manager;
            return manager;
        };
    }
    Module._uapmdEnsureWebclapUiManager().bind(slot, UTF8ToString(container_id));
});

EM_JS(void, uapmd_webclap_unbind_ui_slot, (uint32_t slot), {
    if (Module._uapmdEnsureWebclapUiManager)
        Module._uapmdEnsureWebclapUiManager().unbind(slot);
});

namespace remidy {

// Hardcoded WebCLAP bundle catalog. Each bundle may expose multiple CLAP
// descriptors, so the catalog expands one bundle URL into multiple entries.

struct KnownBundlePlugin {
    const char* clapId;
    const char* displayName;
};

struct KnownBundle {
    const char* url;
    const KnownBundlePlugin* plugins;
    size_t pluginCount;
};

static constexpr KnownBundlePlugin kSignalsmithBasicsPlugins[] = {
    {"uk.co.signalsmith.basics.chorus", "[Basics] Chorus"},
    {"uk.co.signalsmith.basics.limiter", "[Basics] Limiter"},
    {"uk.co.signalsmith.basics.freq-shifter", "[Basics] Frequency Shifter"},
    {"uk.co.signalsmith.basics.analyser", "[Basics] Analyser"},
    {"uk.co.signalsmith.basics.crunch", "[Basics] Crunch"},
    {"uk.co.signalsmith.basics.reverb", "[Basics] Reverb"},
};

static constexpr KnownBundlePlugin kSignalsmithCppExamplePlugins[] = {
    {"uk.co.signalsmith-audio.plugins.example-audio-plugin", "C++ Example Audio Plugin (Chorus)"},
    {"uk.co.signalsmith-audio.plugins.example-note-plugin", "C++ Example Note Plugin"},
    {"uk.co.signalsmith-audio.plugins.example-synth", "C++ Example Synth"},
    {"uk.co.signalsmith-audio.plugins.example-keyboard", "C++ Example Virtual Keyboard"},
};

static constexpr KnownBundle kKnownBundles[] = {
    {
        "https://webclap.github.io/browser-test-host/examples/signalsmith-basics/"
            "basics.wclap.tar.gz",
        kSignalsmithBasicsPlugins,
        std::size(kSignalsmithBasicsPlugins),
    },
    {
        "https://webclap.github.io/browser-test-host/examples/signalsmith-clap-cpp/"
            "example-plugins.wclap.tar.gz",
        kSignalsmithCppExamplePlugins,
        std::size(kSignalsmithCppExamplePlugins),
    },
};

static const char* findKnownBundleUrlForPluginId(std::string_view pluginId) {
    for (const auto& bundle : kKnownBundles) {
        for (size_t i = 0; i < bundle.pluginCount; ++i) {
            if (pluginId == bundle.plugins[i].clapId)
                return bundle.url;
        }
    }
    return nullptr;
}

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
    PerNoteControllerContext context, uint32_t index, double plainValue, uint64_t)
{
    owner_->setCachedParameterValue(index, plainValue);
    std::ostringstream payload;
    payload << "\"index\":" << index
            << ",\"value\":" << plainValue
            << ",\"perNote\":true"
            << ",\"group\":" << context.group
            << ",\"channel\":" << context.channel
            << ",\"note\":" << context.note;
    auto json = buildWclapEventJson("wclap-set-parameter", owner_->slot(), payload.str());
    uapmd_post_to_webclap_worklet_json(json.c_str());

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
    const char* bundleUrlPtr = findKnownBundleUrlForPluginId(clapId);
    std::string bundleUrl = bundleUrlPtr ? std::string{bundleUrlPtr} : info->pluginId();

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
         << ",\"url\":\""  << bundleUrl << "\"";
    if (!clapId.empty() && clapId != bundleUrl)
        json << ",\"pluginId\":\"" << clapId << "\"";
    json
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
    } else if (type == "wclap-ui-info" || type == "wclap-ui-open" || type == "wclap-ui-resize") {
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
            if (type == "wclap-ui-resize")
                instance->notifyUiResizeRequest(ui.canResize, ui.width, ui.height);
        }
    } else if (type == "wclap-parameter-updates") {
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
    } else if (type == "wclap-capabilities") {
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
    } else if (type == "wclap-runtime-error") {
        std::cerr << "[WebCLAP] runtime error: " << message["error"].toString() << std::endl;
    }
    // All other message types (wclap-host-ready, etc.) are silently ignored here.
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
    std::string bundleKey = bundlePath.string();
    std::string formatName = owner_->name();
    for (const auto& bundle : kKnownBundles) {
        if (bundleKey != bundle.url)
            continue;
        for (size_t i = 0; i < bundle.pluginCount; ++i) {
            const auto& plugin = bundle.plugins[i];
            PluginCatalogEntry entry{};
            entry.format(formatName);
            std::string id{plugin.clapId};
            entry.pluginId(id);
            entry.displayName(plugin.displayName);
            entry.bundlePath(bundlePath);
            if (pluginFound)
                pluginFound(std::move(entry));
        }
        break;
    }
    if (scanCompleted)
        scanCompleted("");
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
    for (const auto& bundlePath : enumerateCandidateBundles(false)) {
        bool bundleCompleted = false;
        std::string bundleError;
        scanBundle(bundlePath, false, 0.0, pluginFound,
                   [&](std::string error) {
                       bundleError = std::move(error);
                       bundleCompleted = true;
                   });
        if (!bundleCompleted) {
            if (scanCompleted)
                scanCompleted(std::format("WebCLAP slow scan for '{}' did not invoke completion.", bundlePath.string()));
            return;
        }
        if (!bundleError.empty()) {
            if (scanCompleted)
                scanCompleted(std::move(bundleError));
            return;
        }
    }
    if (scanCompleted)
        scanCompleted("");
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
    std::ostringstream json;
    json << "{\"type\":\"wclap-ui-create\",\"slot\":" << owner_->slot()
         << ",\"width\":" << width_
         << ",\"height\":" << height_
         << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
    created_ = true;
    return true;
}

void PluginInstanceWebCLAP::UISupportWebCLAP::destroy() {
    if (!owner_ || !created_)
        return;
    std::ostringstream json;
    json << "{\"type\":\"wclap-ui-destroy\",\"slot\":" << owner_->slot() << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
    uapmd_webclap_unbind_ui_slot(owner_->slot());
    created_ = false;
    visible_ = false;
}

bool PluginInstanceWebCLAP::UISupportWebCLAP::show() {
    if (!owner_ || !created_)
        return false;
    std::ostringstream json;
    json << "{\"type\":\"wclap-ui-show\",\"slot\":" << owner_->slot() << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
    visible_ = true;
    return true;
}

void PluginInstanceWebCLAP::UISupportWebCLAP::hide() {
    if (!owner_ || !created_)
        return;
    std::ostringstream json;
    json << "{\"type\":\"wclap-ui-hide\",\"slot\":" << owner_->slot() << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
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
    std::ostringstream json;
    json << "{\"type\":\"wclap-ui-set-size\",\"slot\":" << owner_->slot()
         << ",\"width\":" << width
         << ",\"height\":" << height
         << "}";
    uapmd_post_to_webclap_worklet_json(json.str().c_str());
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
