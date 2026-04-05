/* uapmd C API — implementation for the uapmd module bindings */

#include "c-api/uapmd-c-api.h"
#include <uapmd/uapmd.hpp>
#include <cstring>
#include <string>
#include <vector>

/* ── Cast helpers ─────────────────────────────────────────────────────────── */

static uapmd::AudioPluginInstanceAPI* I(uapmd_plugin_instance_t h) { return reinterpret_cast<uapmd::AudioPluginInstanceAPI*>(h); }
static uapmd::AudioPluginHostingAPI*  H(uapmd_plugin_host_t h)     { return reinterpret_cast<uapmd::AudioPluginHostingAPI*>(h); }
static uapmd::AudioPluginGraph*       G(uapmd_plugin_graph_t h)    { return reinterpret_cast<uapmd::AudioPluginGraph*>(h); }
static uapmd::AudioPluginNode*        N(uapmd_plugin_node_t h)     { return reinterpret_cast<uapmd::AudioPluginNode*>(h); }
static uapmd::MidiIOFeature*          M(uapmd_midi_io_t h)         { return reinterpret_cast<uapmd::MidiIOFeature*>(h); }
static uapmd::UapmdFunctionBlock*     FB(uapmd_function_block_t h) { return reinterpret_cast<uapmd::UapmdFunctionBlock*>(h); }
static uapmd::UapmdFunctionBlockManager* FBM(uapmd_function_block_mgr_t h) { return reinterpret_cast<uapmd::UapmdFunctionBlockManager*>(h); }
static uapmd::UapmdFunctionDevice*    FD(uapmd_function_device_t h) { return reinterpret_cast<uapmd::UapmdFunctionDevice*>(h); }
static uapmd::UapmdUmpInputMapper*    UIN(uapmd_ump_input_mapper_t h) { return reinterpret_cast<uapmd::UapmdUmpInputMapper*>(h); }
static uapmd::UapmdUmpOutputMapper*   UOUT(uapmd_ump_output_mapper_t h) { return reinterpret_cast<uapmd::UapmdUmpOutputMapper*>(h); }

/* ── String copy helper ──────────────────────────────────────────────────── */

static size_t copy_string(const std::string& src, char* buf, size_t buf_size) {
    size_t required = src.size() + 1;
    if (!buf || buf_size == 0)
        return required;
    size_t to_copy = (src.size() < buf_size) ? src.size() : (buf_size - 1);
    std::memcpy(buf, src.data(), to_copy);
    buf[to_copy] = '\0';
    return to_copy;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginInstanceAPI
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t uapmd_instance_display_name(uapmd_plugin_instance_t inst, char* buf, size_t buf_size) {
    return copy_string(I(inst)->displayName(), buf, buf_size);
}

size_t uapmd_instance_format_name(uapmd_plugin_instance_t inst, char* buf, size_t buf_size) {
    return copy_string(I(inst)->formatName(), buf, buf_size);
}

size_t uapmd_instance_plugin_id(uapmd_plugin_instance_t inst, char* buf, size_t buf_size) {
    return copy_string(I(inst)->pluginId(), buf, buf_size);
}

bool uapmd_instance_get_bypassed(uapmd_plugin_instance_t inst) { return I(inst)->bypassed(); }
void uapmd_instance_set_bypassed(uapmd_plugin_instance_t inst, bool value) { I(inst)->bypassed(value); }

uapmd_status_t uapmd_instance_start_processing(uapmd_plugin_instance_t inst) { return I(inst)->startProcessing(); }
uapmd_status_t uapmd_instance_stop_processing(uapmd_plugin_instance_t inst)  { return I(inst)->stopProcessing(); }

uint32_t uapmd_instance_latency_in_samples(uapmd_plugin_instance_t inst) { return I(inst)->latencyInSamples(); }
double   uapmd_instance_tail_length_in_seconds(uapmd_plugin_instance_t inst) { return I(inst)->tailLengthInSeconds(); }
bool     uapmd_instance_requires_replacing_process(uapmd_plugin_instance_t inst) { return I(inst)->requiresReplacingProcess(); }

/* ── Parameters ──────────────────────────────────────────────────────────── */

/* Thread-local storage for parameter metadata conversion */
static thread_local std::vector<uapmd::ParameterMetadata> tl_param_list;
static thread_local std::vector<uapmd_parameter_named_value_t> tl_named_values;
static thread_local std::vector<std::string> tl_string_store;

uint32_t uapmd_instance_parameter_count(uapmd_plugin_instance_t inst) {
    tl_param_list = I(inst)->parameterMetadataList();
    return static_cast<uint32_t>(tl_param_list.size());
}

bool uapmd_instance_get_parameter_metadata(uapmd_plugin_instance_t inst, uint32_t list_index, uapmd_parameter_metadata_t* out) {
    if (tl_param_list.empty())
        tl_param_list = I(inst)->parameterMetadataList();
    if (list_index >= tl_param_list.size())
        return false;

    auto& src = tl_param_list[list_index];
    out->index = src.index;
    out->stable_id = src.stableId.c_str();
    out->name = src.name.c_str();
    out->path = src.path.c_str();
    out->default_plain_value = src.defaultPlainValue;
    out->min_plain_value = src.minPlainValue;
    out->max_plain_value = src.maxPlainValue;
    out->automatable = src.automatable;
    out->hidden = src.hidden;
    out->discrete = src.discrete;

    tl_named_values.resize(src.namedValues.size());
    for (size_t i = 0; i < src.namedValues.size(); ++i) {
        tl_named_values[i].value = src.namedValues[i].value;
        tl_named_values[i].name = src.namedValues[i].name.c_str();
    }
    out->named_values_count = static_cast<uint32_t>(tl_named_values.size());
    out->named_values = tl_named_values.data();
    return true;
}

double uapmd_instance_get_parameter_value(uapmd_plugin_instance_t inst, int32_t index) {
    return I(inst)->getParameterValue(index);
}

void uapmd_instance_set_parameter_value(uapmd_plugin_instance_t inst, int32_t index, double value) {
    I(inst)->setParameterValue(index, value);
}

size_t uapmd_instance_get_parameter_value_string(uapmd_plugin_instance_t inst, int32_t index, double value, char* buf, size_t buf_size) {
    auto s = I(inst)->getParameterValueString(index, value);
    return copy_string(s, buf, buf_size);
}

void uapmd_instance_set_per_note_controller_value(uapmd_plugin_instance_t inst, uint8_t note, uint8_t index, double value) {
    I(inst)->setPerNoteControllerValue(note, index, value);
}

size_t uapmd_instance_get_per_note_controller_value_string(uapmd_plugin_instance_t inst, uint8_t note, uint8_t index, double value, char* buf, size_t buf_size) {
    auto s = I(inst)->getPerNoteControllerValueString(note, index, value);
    return copy_string(s, buf, buf_size);
}

/* ── Presets ─────────────────────────────────────────────────────────────── */

static thread_local std::vector<uapmd::PresetsMetadata> tl_preset_list;

uint32_t uapmd_instance_preset_count(uapmd_plugin_instance_t inst) {
    tl_preset_list = I(inst)->presetMetadataList();
    return static_cast<uint32_t>(tl_preset_list.size());
}

bool uapmd_instance_get_preset_metadata(uapmd_plugin_instance_t inst, uint32_t list_index, uapmd_preset_metadata_t* out) {
    if (tl_preset_list.empty())
        tl_preset_list = I(inst)->presetMetadataList();
    if (list_index >= tl_preset_list.size())
        return false;
    auto& src = tl_preset_list[list_index];
    out->bank = src.bank;
    out->index = src.index;
    out->stable_id = src.stableId.c_str();
    out->name = src.name.c_str();
    out->path = src.path.c_str();
    return true;
}

void uapmd_instance_load_preset(uapmd_plugin_instance_t inst, int32_t preset_index) {
    I(inst)->loadPreset(preset_index);
}

/* ── State (sync) ────────────────────────────────────────────────────────── */

static thread_local std::vector<uint8_t> tl_state_buf;

size_t uapmd_instance_save_state_sync(uapmd_plugin_instance_t inst, uint8_t* buf, size_t buf_size) {
    tl_state_buf = I(inst)->saveStateSync();
    if (!buf || buf_size == 0)
        return tl_state_buf.size();
    size_t to_copy = (tl_state_buf.size() < buf_size) ? tl_state_buf.size() : buf_size;
    std::memcpy(buf, tl_state_buf.data(), to_copy);
    return to_copy;
}

void uapmd_instance_load_state_sync(uapmd_plugin_instance_t inst, const uint8_t* data, size_t data_size) {
    std::vector<uint8_t> state(data, data + data_size);
    I(inst)->loadStateSync(state);
}

/* ── State (async) ───────────────────────────────────────────────────────── */

void uapmd_instance_request_state(uapmd_plugin_instance_t inst,
                                   uapmd_state_context_type_t ctx,
                                   bool include_ui_state,
                                   void* user_data,
                                   uapmd_request_state_cb_t callback) {
    I(inst)->requestState(
        static_cast<uapmd::StateContextType>(ctx),
        include_ui_state,
        user_data,
        [callback](std::vector<uint8_t> state, std::string error, void* ud) {
            callback(state.data(), state.size(), error.empty() ? nullptr : error.c_str(), ud);
        });
}

void uapmd_instance_load_state(uapmd_plugin_instance_t inst,
                                const uint8_t* state, size_t state_size,
                                uapmd_state_context_type_t ctx,
                                bool include_ui_state,
                                void* user_data,
                                uapmd_load_state_cb_t callback) {
    std::vector<uint8_t> data(state, state + state_size);
    I(inst)->loadState(
        std::move(data),
        static_cast<uapmd::StateContextType>(ctx),
        include_ui_state,
        user_data,
        [callback](std::string error, void* ud) {
            callback(error.empty() ? nullptr : error.c_str(), ud);
        });
}

/* ── UI ──────────────────────────────────────────────────────────────────── */

bool uapmd_instance_has_ui_support(uapmd_plugin_instance_t inst) { return I(inst)->hasUISupport(); }

bool uapmd_instance_create_ui(uapmd_plugin_instance_t inst,
                               bool is_floating,
                               void* parent_handle,
                               void* resize_user_data,
                               uapmd_ui_resize_handler_t resize_handler) {
    return I(inst)->createUI(is_floating, parent_handle, [resize_handler, resize_user_data](uint32_t w, uint32_t h) -> bool {
        if (resize_handler)
            return resize_handler(w, h, resize_user_data);
        return true;
    });
}

void uapmd_instance_destroy_ui(uapmd_plugin_instance_t inst) { I(inst)->destroyUI(); }
bool uapmd_instance_show_ui(uapmd_plugin_instance_t inst)    { return I(inst)->showUI(); }
void uapmd_instance_hide_ui(uapmd_plugin_instance_t inst)    { I(inst)->hideUI(); }
bool uapmd_instance_is_ui_visible(uapmd_plugin_instance_t inst) { return I(inst)->isUIVisible(); }

bool uapmd_instance_set_ui_size(uapmd_plugin_instance_t inst, uint32_t width, uint32_t height) {
    return I(inst)->setUISize(width, height);
}

bool uapmd_instance_get_ui_size(uapmd_plugin_instance_t inst, uint32_t* width, uint32_t* height) {
    return I(inst)->getUISize(*width, *height);
}

bool uapmd_instance_can_ui_resize(uapmd_plugin_instance_t inst) { return I(inst)->canUIResize(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginHostingAPI
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Stored unique_ptrs for owned hosts */
#include <unordered_map>
#include <mutex>

static std::mutex s_host_mutex;
static std::unordered_map<uapmd::AudioPluginHostingAPI*, std::unique_ptr<uapmd::AudioPluginHostingAPI>> s_owned_hosts;

uapmd_plugin_host_t uapmd_plugin_host_create() {
    auto host = uapmd::AudioPluginHostingAPI::create();
    auto raw = host.get();
    std::lock_guard lock(s_host_mutex);
    s_owned_hosts[raw] = std::move(host);
    return reinterpret_cast<uapmd_plugin_host_t>(raw);
}

void uapmd_plugin_host_destroy(uapmd_plugin_host_t host) {
    std::lock_guard lock(s_host_mutex);
    s_owned_hosts.erase(H(host));
}

static thread_local std::vector<remidy::PluginCatalogEntry> tl_catalog;

uint32_t uapmd_plugin_host_catalog_entry_count(uapmd_plugin_host_t host) {
    tl_catalog = H(host)->pluginCatalogEntries();
    return static_cast<uint32_t>(tl_catalog.size());
}

bool uapmd_plugin_host_get_catalog_entry(uapmd_plugin_host_t host, uint32_t index,
                                          char* format_buf, size_t format_buf_size,
                                          char* plugin_id_buf, size_t plugin_id_buf_size,
                                          char* display_name_buf, size_t display_name_buf_size) {
    if (tl_catalog.empty())
        tl_catalog = H(host)->pluginCatalogEntries();
    if (index >= tl_catalog.size())
        return false;
    auto& entry = tl_catalog[index];
    copy_string(entry.format(), format_buf, format_buf_size);
    copy_string(entry.pluginId(), plugin_id_buf, plugin_id_buf_size);
    copy_string(entry.displayName(), display_name_buf, display_name_buf_size);
    return true;
}

void uapmd_plugin_host_save_catalog(uapmd_plugin_host_t host, const char* path) {
    H(host)->savePluginCatalogToFile(path);
}

void uapmd_plugin_host_perform_scanning(uapmd_plugin_host_t host, bool rescan) {
    H(host)->performPluginScanning(rescan);
}

void uapmd_plugin_host_reload_catalog_from_cache(uapmd_plugin_host_t host) {
    H(host)->reloadPluginCatalogFromCache();
}

void uapmd_plugin_host_create_instance(uapmd_plugin_host_t host,
                                        uint32_t sample_rate,
                                        uint32_t buffer_size,
                                        int32_t main_input_channels,
                                        int32_t main_output_channels,
                                        bool offline_mode,
                                        const char* format,
                                        const char* plugin_id,
                                        void* user_data,
                                        uapmd_create_instance_cb_t callback) {
    std::string fmt = format;
    std::string pid = plugin_id;
    std::optional<uint32_t> inCh = main_input_channels >= 0 ? std::optional<uint32_t>(main_input_channels) : std::nullopt;
    std::optional<uint32_t> outCh = main_output_channels >= 0 ? std::optional<uint32_t>(main_output_channels) : std::nullopt;
    H(host)->createPluginInstance(sample_rate, buffer_size, inCh, outCh, offline_mode, fmt, pid,
        [callback, user_data](int32_t instanceId, std::string error) {
            callback(instanceId, error.empty() ? nullptr : error.c_str(), user_data);
        });
}

void uapmd_plugin_host_delete_instance(uapmd_plugin_host_t host, int32_t instance_id) {
    H(host)->deletePluginInstance(instance_id);
}

uapmd_plugin_instance_t uapmd_plugin_host_get_instance(uapmd_plugin_host_t host, int32_t instance_id) {
    return reinterpret_cast<uapmd_plugin_instance_t>(H(host)->getInstance(instance_id));
}

static thread_local std::vector<int32_t> tl_instance_ids;

uint32_t uapmd_plugin_host_instance_id_count(uapmd_plugin_host_t host) {
    tl_instance_ids = H(host)->instanceIds();
    return static_cast<uint32_t>(tl_instance_ids.size());
}

bool uapmd_plugin_host_get_instance_ids(uapmd_plugin_host_t host, int32_t* out, uint32_t out_count) {
    if (tl_instance_ids.empty())
        tl_instance_ids = H(host)->instanceIds();
    if (out_count < tl_instance_ids.size())
        return false;
    std::memcpy(out, tl_instance_ids.data(), tl_instance_ids.size() * sizeof(int32_t));
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginNode
 * ═══════════════════════════════════════════════════════════════════════════ */

int32_t uapmd_node_instance_id(uapmd_plugin_node_t node) { return N(node)->instanceId(); }

uapmd_plugin_instance_t uapmd_node_instance(uapmd_plugin_node_t node) {
    return reinterpret_cast<uapmd_plugin_instance_t>(N(node)->instance());
}

bool uapmd_node_schedule_events(uapmd_plugin_node_t node, uapmd_timestamp_t timestamp, void* events, size_t size) {
    return N(node)->scheduleEvents(timestamp, events, size);
}

void uapmd_node_send_all_notes_off(uapmd_plugin_node_t node) { N(node)->sendAllNotesOff(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginGraph
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::mutex s_graph_mutex;
static std::unordered_map<uapmd::AudioPluginGraph*, std::unique_ptr<uapmd::AudioPluginGraph>> s_owned_graphs;

uapmd_plugin_graph_t uapmd_graph_create(size_t event_buffer_size_in_bytes) {
    auto graph = uapmd::AudioPluginGraph::create(event_buffer_size_in_bytes);
    auto raw = graph.get();
    std::lock_guard lock(s_graph_mutex);
    s_owned_graphs[raw] = std::move(graph);
    return reinterpret_cast<uapmd_plugin_graph_t>(raw);
}

void uapmd_graph_destroy(uapmd_plugin_graph_t graph) {
    std::lock_guard lock(s_graph_mutex);
    s_owned_graphs.erase(G(graph));
}

/* Store C callback user_data pairs for graph on-delete callbacks */
struct GraphDeleteCbData {
    void* user_data;
    uapmd_graph_delete_cb_t callback;
};

uapmd_status_t uapmd_graph_append_node(uapmd_plugin_graph_t graph,
                                        int32_t instance_id,
                                        uapmd_plugin_instance_t instance,
                                        void* delete_user_data,
                                        uapmd_graph_delete_cb_t on_delete) {
    return G(graph)->appendNodeSimple(instance_id, I(instance), [on_delete, delete_user_data]() {
        if (on_delete)
            on_delete(delete_user_data);
    });
}

bool uapmd_graph_remove_node(uapmd_plugin_graph_t graph, int32_t instance_id) {
    return G(graph)->removeNodeSimple(instance_id);
}

uint32_t uapmd_graph_plugin_count(uapmd_plugin_graph_t graph) {
    return static_cast<uint32_t>(G(graph)->plugins().size());
}

uapmd_plugin_node_t uapmd_graph_get_plugin_node(uapmd_plugin_graph_t graph, int32_t instance_id) {
    return reinterpret_cast<uapmd_plugin_node_t>(G(graph)->getPluginNode(instance_id));
}

void uapmd_graph_set_event_output_callback(uapmd_plugin_graph_t graph,
                                            void* user_data,
                                            uapmd_event_output_cb_t callback) {
    G(graph)->setEventOutputCallback([callback, user_data](int32_t instanceId, const uapmd_ump_t* data, size_t dataSizeInBytes) {
        if (callback)
            callback(instanceId, data, dataSizeInBytes, user_data);
    });
}

uint32_t uapmd_graph_output_bus_count(uapmd_plugin_graph_t graph)                            { return G(graph)->outputBusCount(); }
uint32_t uapmd_graph_output_latency_in_samples(uapmd_plugin_graph_t graph, uint32_t idx)    { return G(graph)->outputLatencyInSamples(idx); }
double   uapmd_graph_output_tail_length_in_seconds(uapmd_plugin_graph_t graph, uint32_t idx) { return G(graph)->outputTailLengthInSeconds(idx); }
uint32_t uapmd_graph_render_lead_in_samples(uapmd_plugin_graph_t graph)                      { return G(graph)->renderLeadInSamples(); }
uint32_t uapmd_graph_main_output_latency_in_samples(uapmd_plugin_graph_t graph)              { return G(graph)->mainOutputLatencyInSamples(); }
double   uapmd_graph_main_output_tail_length_in_seconds(uapmd_plugin_graph_t graph)          { return G(graph)->mainOutputTailLengthInSeconds(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiIOFeature
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_midi_io_add_input_handler(uapmd_midi_io_t io, uapmd_ump_receiver_t receiver, void* user_data) {
    M(io)->addInputHandler(receiver, user_data);
}

void uapmd_midi_io_remove_input_handler(uapmd_midi_io_t io, uapmd_ump_receiver_t receiver) {
    M(io)->removeInputHandler(receiver);
}

void uapmd_midi_io_send(uapmd_midi_io_t io, uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) {
    M(io)->send(messages, length, timestamp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdFunctionBlock
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_midi_io_t uapmd_fb_midi_io(uapmd_function_block_t fb)     { return reinterpret_cast<uapmd_midi_io_t>(FB(fb)->midiIO()); }
int32_t  uapmd_fb_instance_id(uapmd_function_block_t fb)        { return FB(fb)->instanceId(); }
uint8_t  uapmd_fb_get_group(uapmd_function_block_t fb)          { return FB(fb)->group(); }
void     uapmd_fb_set_group(uapmd_function_block_t fb, uint8_t gid) { FB(fb)->group(gid); }
void     uapmd_fb_detach_output_mapper(uapmd_function_block_t fb)   { FB(fb)->detachOutputMapper(); }
void     uapmd_fb_initialize(uapmd_function_block_t fb)             { FB(fb)->initialize(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdFunctionBlockManager
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t uapmd_fbm_count(uapmd_function_block_mgr_t mgr) { return FBM(mgr)->count(); }
size_t uapmd_fbm_create_device(uapmd_function_block_mgr_t mgr) { return FBM(mgr)->create(); }

uapmd_function_device_t uapmd_fbm_get_device_by_index(uapmd_function_block_mgr_t mgr, int32_t index) {
    return reinterpret_cast<uapmd_function_device_t>(FBM(mgr)->getFunctionDeviceByIndex(index));
}

uapmd_function_device_t uapmd_fbm_get_device_for_instance(uapmd_function_block_mgr_t mgr, int32_t instance_id) {
    return reinterpret_cast<uapmd_function_device_t>(FBM(mgr)->getFunctionDeviceForInstance(instance_id));
}

void uapmd_fbm_delete_empty_devices(uapmd_function_block_mgr_t mgr)   { FBM(mgr)->deleteEmptyDevices(); }
void uapmd_fbm_detach_all_output_mappers(uapmd_function_block_mgr_t mgr) { FBM(mgr)->detachAllOutputMappers(); }
void uapmd_fbm_clear_all_devices(uapmd_function_block_mgr_t mgr)      { FBM(mgr)->clearAllDevices(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  UmpInputMapper / UmpOutputMapper
 * ═══════════════════════════════════════════════════════════════════════════ */

void   uapmd_ump_in_set_parameter_value(uapmd_ump_input_mapper_t m, uint16_t index, double value)     { UIN(m)->setParameterValue(index, value); }
double uapmd_ump_in_get_parameter_value(uapmd_ump_input_mapper_t m, uint16_t index)                   { return UIN(m)->getParameterValue(index); }
void   uapmd_ump_in_set_per_note_controller_value(uapmd_ump_input_mapper_t m, uint8_t note, uint8_t index, double value) { UIN(m)->setPerNoteControllerValue(note, index, value); }
void   uapmd_ump_in_load_preset(uapmd_ump_input_mapper_t m, uint32_t index)                           { UIN(m)->loadPreset(index); }

void uapmd_ump_out_send_parameter_value(uapmd_ump_output_mapper_t m, uint16_t index, double value)     { UOUT(m)->sendParameterValue(index, value); }
void uapmd_ump_out_send_per_note_controller_value(uapmd_ump_output_mapper_t m, uint8_t note, uint8_t index, double value) { UOUT(m)->sendPerNoteControllerValue(note, index, value); }
void uapmd_ump_out_send_preset_index_change(uapmd_ump_output_mapper_t m, uint32_t index)               { UOUT(m)->sendPresetIndexChange(index); }
