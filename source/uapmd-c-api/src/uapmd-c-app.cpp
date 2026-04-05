/* uapmd C API — implementation for AppModel bindings */

#include "c-api/uapmd-c-app.h"
#include "c-api-internal.h"
#include "AppModel.hpp"
#include <uapmd/uapmd.hpp>
#include <cstring>
#include <string>
#include <vector>

/* ── Cast helpers ─────────────────────────────────────────────────────────── */

static uapmd::AppModel*             AM(uapmd_app_model_t h)            { return reinterpret_cast<uapmd::AppModel*>(h); }
static uapmd::TransportController*   TC(uapmd_transport_controller_t h) { return reinterpret_cast<uapmd::TransportController*>(h); }

static size_t copy_string(const std::string& src, char* buf, size_t buf_size) {
    size_t required = src.size() + 1;
    if (!buf || buf_size == 0)
        return required;
    size_t to_copy = (src.size() < buf_size) ? src.size() : (buf_size - 1);
    std::memcpy(buf, src.data(), to_copy);
    buf[to_copy] = '\0';
    return to_copy;
}

static uapmd::TimelinePosition to_cpp(uapmd_timeline_position_t p) {
    uapmd::TimelinePosition pos;
    pos.samples = p.samples;
    pos.legacy_beats = p.legacy_beats;
    return pos;
}

static uapmd_timeline_position_t to_c(const uapmd::TimelinePosition& p) {
    return { p.samples, p.legacy_beats };
}

static thread_local std::string tl_error;
static thread_local std::string tl_error2;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_instantiate() { uapmd::AppModel::instantiate(); }

uapmd_app_model_t uapmd_app_instance() {
    return reinterpret_cast<uapmd_app_model_t>(&uapmd::AppModel::instance());
}

void uapmd_app_cleanup() { uapmd::AppModel::cleanupInstance(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  Accessors
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_realtime_sequencer_t uapmd_app_sequencer(uapmd_app_model_t app) {
    return reinterpret_cast<uapmd_realtime_sequencer_t>(&AM(app)->sequencer());
}

uapmd_transport_controller_t uapmd_app_transport(uapmd_app_model_t app) {
    return reinterpret_cast<uapmd_transport_controller_t>(&AM(app)->transport());
}

uapmd_document_provider_t uapmd_app_document_provider(uapmd_app_model_t app) {
    return reinterpret_cast<uapmd_document_provider_t>(AM(app)->documentProvider());
}

int32_t uapmd_app_sample_rate(uapmd_app_model_t app) { return AM(app)->sampleRate(); }
uint32_t uapmd_app_track_count(uapmd_app_model_t app) { return static_cast<uint32_t>(AM(app)->trackCount()); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  Audio engine control
 * ═══════════════════════════════════════════════════════════════════════════ */

bool uapmd_app_is_scanning(uapmd_app_model_t app)                  { return AM(app)->isScanning(); }
bool uapmd_app_is_audio_engine_enabled(uapmd_app_model_t app)      { return AM(app)->isAudioEngineEnabled(); }
void uapmd_app_set_audio_engine_enabled(uapmd_app_model_t app, bool en) { AM(app)->setAudioEngineEnabled(en); }
void uapmd_app_toggle_audio_engine(uapmd_app_model_t app)          { AM(app)->toggleAudioEngine(); }
void uapmd_app_update_audio_device_settings(uapmd_app_model_t app, int32_t sr, uint32_t bs) { AM(app)->updateAudioDeviceSettings(sr, bs); }
void uapmd_app_set_auto_buffer_size_enabled(uapmd_app_model_t app, bool en) { AM(app)->setAutoBufferSizeEnabled(en); }
bool uapmd_app_auto_buffer_size_enabled(uapmd_app_model_t app)     { return AM(app)->autoBufferSizeEnabled(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin scanning
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_perform_plugin_scanning(uapmd_app_model_t app,
                                        bool force_rescan,
                                        uapmd_plugin_scan_request_t request,
                                        double remote_timeout_seconds,
                                        bool require_fast_scanning) {
    AM(app)->performPluginScanning(force_rescan,
        static_cast<uapmd::AppModel::PluginScanRequest>(request),
        remote_timeout_seconds, require_fast_scanning);
}

void uapmd_app_cancel_plugin_scanning(uapmd_app_model_t app) { AM(app)->cancelPluginScanning(); }

size_t uapmd_app_generate_scan_report(uapmd_app_model_t app, char* buf, size_t buf_size) {
    auto report = AM(app)->generateScanReport();
    return copy_string(report, buf, buf_size);
}

void uapmd_app_clear_plugin_blocklist(uapmd_app_model_t app) { AM(app)->clearPluginBlocklist(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin instance management
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_create_plugin_instance(uapmd_app_model_t app,
                                       const char* format,
                                       const char* plugin_id,
                                       int32_t track_index,
                                       const uapmd_plugin_instance_config_t* config,
                                       void* user_data,
                                       uapmd_instance_created_cb_t callback) {
    uapmd::AppModel::PluginInstanceConfig cfg;
    if (config) {
        if (config->api_name) cfg.apiName = config->api_name;
        if (config->device_name) cfg.deviceName = config->device_name;
        if (config->manufacturer) cfg.manufacturer = config->manufacturer;
        if (config->version) cfg.version = config->version;
        if (config->state_file) cfg.stateFile = config->state_file;
    }

    AM(app)->createPluginInstanceAsync(format, plugin_id, track_index, cfg,
        [callback, user_data](const uapmd::AppModel::PluginInstanceResult& r) {
            if (!callback) return;
            uapmd_plugin_instance_result_t cr;
            cr.instance_id = r.instanceId;
            cr.plugin_name = r.pluginName.c_str();
            cr.error = r.error.empty() ? nullptr : r.error.c_str();
            callback(cr, user_data);
        });
}

void uapmd_app_remove_plugin_instance(uapmd_app_model_t app, int32_t instance_id) {
    AM(app)->removePluginInstance(instance_id);
}

uint8_t uapmd_app_get_instance_group(uapmd_app_model_t app, int32_t instance_id) {
    return AM(app)->getInstanceGroup(instance_id);
}

bool uapmd_app_set_instance_group(uapmd_app_model_t app, int32_t instance_id, uint8_t group) {
    return AM(app)->setInstanceGroup(instance_id, group);
}

void uapmd_app_enable_ump_device(uapmd_app_model_t app, int32_t instance_id, const char* device_name) {
    AM(app)->enableUmpDevice(instance_id, device_name ? device_name : "");
}

void uapmd_app_disable_ump_device(uapmd_app_model_t app, int32_t instance_id) {
    AM(app)->disableUmpDevice(instance_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin UI
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_request_show_plugin_ui(uapmd_app_model_t app, int32_t instance_id) {
    AM(app)->requestShowPluginUI(instance_id);
}

void uapmd_app_show_plugin_ui(uapmd_app_model_t app,
                                int32_t instance_id,
                                bool needs_create,
                                bool is_floating,
                                void* parent_handle,
                                void* resize_user_data,
                                uapmd_ui_resize_handler_t resize_handler) {
    AM(app)->showPluginUI(instance_id, needs_create, is_floating, parent_handle,
        [resize_handler, resize_user_data](uint32_t w, uint32_t h) -> bool {
            if (resize_handler)
                return resize_handler(w, h, resize_user_data);
            return true;
        });
}

void uapmd_app_hide_plugin_ui(uapmd_app_model_t app, int32_t instance_id) {
    AM(app)->hidePluginUI(instance_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin state save/load
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_load_plugin_state(uapmd_app_model_t app,
                                   int32_t instance_id,
                                   const char* filepath,
                                   void* user_data,
                                   uapmd_plugin_state_cb_t callback) {
    AM(app)->loadPluginState(instance_id, filepath,
        [callback, user_data](uapmd::AppModel::PluginStateResult r) {
            if (!callback) return;
            uapmd_plugin_state_result_t cr;
            cr.instance_id = r.instanceId;
            cr.success = r.success;
            cr.error = r.error.empty() ? nullptr : r.error.c_str();
            cr.filepath = r.filepath.empty() ? nullptr : r.filepath.c_str();
            callback(cr, user_data);
        });
}

void uapmd_app_save_plugin_state(uapmd_app_model_t app,
                                   int32_t instance_id,
                                   const char* filepath,
                                   void* user_data,
                                   uapmd_plugin_state_cb_t callback) {
    AM(app)->savePluginState(instance_id, filepath,
        [callback, user_data](uapmd::AppModel::PluginStateResult r) {
            if (!callback) return;
            uapmd_plugin_state_result_t cr;
            cr.instance_id = r.instanceId;
            cr.success = r.success;
            cr.error = r.error.empty() ? nullptr : r.error.c_str();
            cr.filepath = r.filepath.empty() ? nullptr : r.filepath.c_str();
            callback(cr, user_data);
        });
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Clip management
 * ═══════════════════════════════════════════════════════════════════════════ */

static uapmd_clip_add_result_t to_c_clip_result(const uapmd::AppModel::ClipAddResult& r) {
    tl_error = r.error;
    return { r.clipId, r.sourceNodeId, r.success, tl_error.empty() ? nullptr : tl_error.c_str() };
}

uapmd_clip_add_result_t uapmd_app_add_clip_to_track(uapmd_app_model_t app,
                                                      int32_t track_index,
                                                      uapmd_timeline_position_t position,
                                                      uapmd_audio_file_reader_t reader,
                                                      const char* filepath) {
    /* Transfer ownership from the C registry */

    auto* raw = reinterpret_cast<uapmd::AudioFileReader*>(reader);
    std::unique_ptr<uapmd::AudioFileReader> owned;
    {
        std::lock_guard lock(s_reader_mutex);
        auto it = s_owned_readers.find(raw);
        if (it != s_owned_readers.end()) {
            owned = std::move(it->second);
            s_owned_readers.erase(it);
        }
    }
    if (!owned)
        return { -1, -1, false, "reader not found or already consumed" };

    auto r = AM(app)->addClipToTrack(track_index, to_cpp(position), std::move(owned), filepath ? filepath : "");
    return to_c_clip_result(r);
}

uapmd_clip_add_result_t uapmd_app_add_midi_clip_to_track(uapmd_app_model_t app,
                                                           int32_t track_index,
                                                           uapmd_timeline_position_t position,
                                                           const char* filepath) {
    auto r = AM(app)->addMidiClipToTrack(track_index, to_cpp(position), filepath);
    return to_c_clip_result(r);
}

uapmd_clip_add_result_t uapmd_app_add_midi_clip_from_data(uapmd_app_model_t app,
                                                            int32_t track_index,
                                                            uapmd_timeline_position_t position,
                                                            const uapmd_ump_t* ump_events,
                                                            uint32_t ump_event_count,
                                                            const uint64_t* tick_timestamps,
                                                            uint32_t tick_count,
                                                            uint32_t tick_resolution,
                                                            double clip_tempo,
                                                            const uapmd_midi_tempo_change_t* tempo_changes,
                                                            uint32_t tempo_change_count,
                                                            const uapmd_midi_time_sig_change_t* time_sig_changes,
                                                            uint32_t time_sig_change_count,
                                                            const char* clip_name,
                                                            bool needs_file_save) {
    std::vector<uapmd_ump_t> ump(ump_events, ump_events + ump_event_count);
    std::vector<uint64_t> ticks(tick_timestamps, tick_timestamps + tick_count);
    std::vector<uapmd::MidiTempoChange> tc(tempo_change_count);
    for (uint32_t i = 0; i < tempo_change_count; ++i) {
        tc[i].tickPosition = tempo_changes[i].tick_position;
        tc[i].bpm = tempo_changes[i].bpm;
    }
    std::vector<uapmd::MidiTimeSignatureChange> tsc(time_sig_change_count);
    for (uint32_t i = 0; i < time_sig_change_count; ++i) {
        tsc[i].tickPosition = time_sig_changes[i].tick_position;
        tsc[i].numerator = time_sig_changes[i].numerator;
        tsc[i].denominator = time_sig_changes[i].denominator;
        tsc[i].clocksPerClick = time_sig_changes[i].clocks_per_click;
        tsc[i].thirtySecondsPerQuarter = time_sig_changes[i].thirty_seconds_per_quarter;
    }

    auto r = AM(app)->addMidiClipToTrack(track_index, to_cpp(position),
        std::move(ump), std::move(ticks), tick_resolution, clip_tempo,
        std::move(tc), std::move(tsc),
        clip_name ? clip_name : "", needs_file_save);
    return to_c_clip_result(r);
}

uapmd_clip_add_result_t uapmd_app_create_empty_midi_clip(uapmd_app_model_t app,
                                                           int32_t track_index,
                                                           int64_t position_samples,
                                                           uint32_t tick_resolution,
                                                           double bpm) {
    auto r = AM(app)->createEmptyMidiClip(track_index, position_samples, tick_resolution, bpm);
    return to_c_clip_result(r);
}

bool uapmd_app_remove_clip_from_track(uapmd_app_model_t app, int32_t track_index, int32_t clip_id) {
    return AM(app)->removeClipFromTrack(track_index, clip_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Track management
 * ═══════════════════════════════════════════════════════════════════════════ */

int32_t uapmd_app_add_track(uapmd_app_model_t app)                         { return AM(app)->addTrack(); }
bool    uapmd_app_remove_track(uapmd_app_model_t app, int32_t track_index) { return AM(app)->removeTrack(track_index); }
void    uapmd_app_remove_all_tracks(uapmd_app_model_t app)                 { AM(app)->removeAllTracks(); }

int32_t uapmd_app_add_device_input_to_track(uapmd_app_model_t app,
                                              int32_t track_index,
                                              const uint32_t* channel_indices,
                                              uint32_t channel_count) {
    std::vector<uint32_t> indices(channel_indices, channel_indices + channel_count);
    return AM(app)->addDeviceInputToTrack(track_index, indices);
}

uint32_t uapmd_app_timeline_track_count(uapmd_app_model_t app) {
    return static_cast<uint32_t>(AM(app)->getTimelineTracks().size());
}

uapmd_timeline_track_t uapmd_app_get_timeline_track(uapmd_app_model_t app, uint32_t index) {
    auto tracks = AM(app)->getTimelineTracks();
    if (index >= tracks.size()) return nullptr;
    return reinterpret_cast<uapmd_timeline_track_t>(tracks[index]);
}

uapmd_timeline_track_t uapmd_app_master_timeline_track(uapmd_app_model_t app) {
    return reinterpret_cast<uapmd_timeline_track_t>(AM(app)->getMasterTimelineTrack());
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Timeline state
 * ═══════════════════════════════════════════════════════════════════════════ */

bool uapmd_app_get_timeline_state(uapmd_app_model_t app, uapmd_timeline_state_t* out) {
    auto& st = AM(app)->timeline();
    out->playhead_position = to_c(st.playheadPosition);
    out->is_playing = st.isPlaying;
    out->loop_enabled = st.loopEnabled;
    out->loop_start = to_c(st.loopStart);
    out->loop_end = to_c(st.loopEnd);
    out->tempo = st.tempo;
    out->time_signature_numerator = st.timeSignatureNumerator;
    out->time_signature_denominator = st.timeSignatureDenominator;
    out->sample_rate = st.sample_rate;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Instance details
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_request_show_instance_details(uapmd_app_model_t app, int32_t instance_id) {
    AM(app)->requestShowInstanceDetails(instance_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Track graph editing (DAG)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uapmd_graph_endpoint_t to_c_endpoint(const uapmd::AudioPluginGraphEndpoint& e) {
    return {
        static_cast<uapmd_graph_endpoint_type_t>(e.type),
        e.instance_id,
        e.bus_index
    };
}

static uapmd::AudioPluginGraphEndpoint to_cpp_endpoint(const uapmd_graph_endpoint_t& e) {
    uapmd::AudioPluginGraphEndpoint r;
    r.type = static_cast<uapmd::AudioPluginGraphEndpointType>(e.type);
    r.instance_id = e.instance_id;
    r.bus_index = e.bus_index;
    return r;
}

bool uapmd_app_ensure_track_uses_editor_graph(uapmd_app_model_t app, int32_t track_index) {
    return AM(app)->ensureTrackUsesEditorGraph(track_index);
}

void uapmd_app_request_show_track_graph(uapmd_app_model_t app, int32_t track_index) {
    AM(app)->requestShowTrackGraph(track_index);
}

bool uapmd_app_revert_track_to_simple_graph(uapmd_app_model_t app, int32_t track_index) {
    return AM(app)->revertTrackToSimpleGraph(track_index);
}

static thread_local std::vector<uapmd_graph_connection_t> tl_connections;
static thread_local std::string tl_conn_error;

uapmd_graph_connections_result_t uapmd_app_get_track_graph_connections(uapmd_app_model_t app, int32_t track_index) {
    std::vector<uapmd::AudioPluginGraphConnection> conns;
    tl_conn_error.clear();
    bool ok = AM(app)->getTrackGraphConnections(track_index, conns, tl_conn_error);
    tl_connections.clear();
    tl_connections.reserve(conns.size());
    for (auto& c : conns)
        tl_connections.push_back({c.id, static_cast<uapmd_graph_bus_type_t>(c.bus_type),
                                  to_c_endpoint(c.source), to_c_endpoint(c.target)});
    return {
        ok,
        tl_conn_error.empty() ? nullptr : tl_conn_error.c_str(),
        static_cast<uint32_t>(tl_connections.size()),
        tl_connections.data()
    };
}

uapmd_op_result_t uapmd_app_connect_track_graph(uapmd_app_model_t app,
                                                  int32_t track_index,
                                                  const uapmd_graph_connection_t* connection) {
    uapmd::AudioPluginGraphConnection c;
    c.id = connection->id;
    c.bus_type = static_cast<uapmd::AudioPluginGraphBusType>(connection->bus_type);
    c.source = to_cpp_endpoint(connection->source);
    c.target = to_cpp_endpoint(connection->target);
    tl_conn_error.clear();
    bool ok = AM(app)->connectTrackGraph(track_index, c, tl_conn_error);
    return { ok, tl_conn_error.empty() ? nullptr : tl_conn_error.c_str() };
}

uapmd_op_result_t uapmd_app_disconnect_track_graph_connection(uapmd_app_model_t app,
                                                               int32_t track_index,
                                                               int64_t connection_id) {
    tl_conn_error.clear();
    bool ok = AM(app)->disconnectTrackGraphConnection(track_index, connection_id, tl_conn_error);
    return { ok, tl_conn_error.empty() ? nullptr : tl_conn_error.c_str() };
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Clip audio events (markers + warps)
 * ═══════════════════════════════════════════════════════════════════════════ */

static thread_local std::vector<uapmd::ClipMarker> tl_clip_markers;
static thread_local std::vector<uapmd::AudioWarpPoint> tl_clip_warps;
static thread_local std::vector<uapmd_clip_marker_t> tl_clip_markers_c;
static thread_local std::vector<uapmd_audio_warp_point_t> tl_clip_warps_c;
static thread_local std::string tl_clip_events_error;

uapmd_clip_audio_events_result_t uapmd_app_get_clip_audio_events(uapmd_app_model_t app,
                                                                   int32_t track_index,
                                                                   int32_t clip_id) {
    tl_clip_markers.clear();
    tl_clip_warps.clear();
    tl_clip_events_error.clear();
    bool ok = AM(app)->getClipAudioEvents(track_index, clip_id, tl_clip_markers, tl_clip_warps, tl_clip_events_error);

    tl_clip_markers_c.clear();
    for (auto& m : tl_clip_markers)
        tl_clip_markers_c.push_back({
            m.markerId.c_str(), m.clipPositionOffset,
            static_cast<uapmd_audio_warp_reference_type_t>(m.referenceType),
            m.referenceClipId.c_str(), m.referenceMarkerId.c_str(), m.name.c_str()
        });

    tl_clip_warps_c.clear();
    for (auto& w : tl_clip_warps)
        tl_clip_warps_c.push_back({
            w.clipPositionOffset, w.speedRatio,
            static_cast<uapmd_audio_warp_reference_type_t>(w.referenceType),
            w.referenceClipId.c_str(), w.referenceMarkerId.c_str()
        });

    return {
        ok, tl_clip_events_error.empty() ? nullptr : tl_clip_events_error.c_str(),
        static_cast<uint32_t>(tl_clip_markers_c.size()), tl_clip_markers_c.data(),
        static_cast<uint32_t>(tl_clip_warps_c.size()), tl_clip_warps_c.data()
    };
}

static std::vector<uapmd::ClipMarker> markers_from_c(const uapmd_clip_marker_t* markers, uint32_t count) {
    std::vector<uapmd::ClipMarker> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uapmd::ClipMarker m;
        if (markers[i].marker_id) m.markerId = markers[i].marker_id;
        m.clipPositionOffset = markers[i].clip_position_offset;
        m.referenceType = static_cast<uapmd::AudioWarpReferenceType>(markers[i].reference_type);
        if (markers[i].reference_clip_id) m.referenceClipId = markers[i].reference_clip_id;
        if (markers[i].reference_marker_id) m.referenceMarkerId = markers[i].reference_marker_id;
        if (markers[i].name) m.name = markers[i].name;
        result.push_back(std::move(m));
    }
    return result;
}

static std::vector<uapmd::AudioWarpPoint> warps_from_c(const uapmd_audio_warp_point_t* warps, uint32_t count) {
    std::vector<uapmd::AudioWarpPoint> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uapmd::AudioWarpPoint w;
        w.clipPositionOffset = warps[i].clip_position_offset;
        w.speedRatio = warps[i].speed_ratio;
        w.referenceType = static_cast<uapmd::AudioWarpReferenceType>(warps[i].reference_type);
        if (warps[i].reference_clip_id) w.referenceClipId = warps[i].reference_clip_id;
        if (warps[i].reference_marker_id) w.referenceMarkerId = warps[i].reference_marker_id;
        result.push_back(std::move(w));
    }
    return result;
}

uapmd_op_result_t uapmd_app_set_clip_audio_events(uapmd_app_model_t app,
                                                     int32_t track_index,
                                                     int32_t clip_id,
                                                     const uapmd_clip_marker_t* markers,
                                                     uint32_t marker_count,
                                                     const uapmd_audio_warp_point_t* warps,
                                                     uint32_t warp_count) {
    tl_clip_events_error.clear();
    bool ok = AM(app)->setClipAudioEvents(track_index, clip_id,
        markers_from_c(markers, marker_count),
        warps_from_c(warps, warp_count),
        tl_clip_events_error);
    return { ok, tl_clip_events_error.empty() ? nullptr : tl_clip_events_error.c_str() };
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Master track markers
 * ═══════════════════════════════════════════════════════════════════════════ */

static thread_local std::vector<uapmd_clip_marker_t> tl_master_markers_c;

uint32_t uapmd_app_master_marker_count(uapmd_app_model_t app) {
    auto& markers = AM(app)->masterTrackMarkers();
    tl_master_markers_c.clear();
    for (auto& m : markers)
        tl_master_markers_c.push_back({
            m.markerId.c_str(), m.clipPositionOffset,
            static_cast<uapmd_audio_warp_reference_type_t>(m.referenceType),
            m.referenceClipId.c_str(), m.referenceMarkerId.c_str(), m.name.c_str()
        });
    return static_cast<uint32_t>(tl_master_markers_c.size());
}

bool uapmd_app_get_master_marker(uapmd_app_model_t app, uint32_t index, uapmd_clip_marker_t* out) {
    (void) app; /* data already cached from master_marker_count call */
    if (index >= tl_master_markers_c.size()) return false;
    *out = tl_master_markers_c[index];
    return true;
}

uapmd_op_result_t uapmd_app_set_master_markers(uapmd_app_model_t app,
                                                 const uapmd_clip_marker_t* markers,
                                                 uint32_t count) {
    tl_clip_events_error.clear();
    bool ok = AM(app)->setMasterTrackMarkersWithValidation(markers_from_c(markers, count), tl_clip_events_error);
    return { ok, tl_clip_events_error.empty() ? nullptr : tl_clip_events_error.c_str() };
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MIDI clip UMP event editing
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * getMidiClipUmpEvents returns a choc::value::Value (JSON-like). We convert it
 * into a flat C-friendly array of uapmd_ump_event_t. The choc::value contains
 * an "events" array of { tick, words: [...] } objects.
 */
static thread_local std::vector<uapmd_ump_event_t> tl_ump_events;
static thread_local std::vector<std::vector<uint32_t>> tl_ump_words_storage;
static thread_local std::string tl_ump_error;

uapmd_ump_events_result_t uapmd_app_get_midi_clip_ump_events(uapmd_app_model_t app,
                                                               int32_t track_index,
                                                               int32_t clip_id) {
    tl_ump_events.clear();
    tl_ump_words_storage.clear();
    tl_ump_error.clear();

    auto val = AM(app)->getMidiClipUmpEvents(track_index, clip_id);
    if (!val.isObject() || !val.hasObjectMember("events"))
        return { false, "invalid result", 0, nullptr };

    auto events = val["events"];
    auto count = events.size();
    tl_ump_events.reserve(count);
    tl_ump_words_storage.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        auto ev = events[i];
        uint64_t tick = ev.hasObjectMember("tick") ? static_cast<uint64_t>(ev["tick"].getInt64()) : 0;
        auto wordsVal = ev["words"];
        std::vector<uint32_t> words;
        for (uint32_t j = 0; j < wordsVal.size(); ++j)
            words.push_back(static_cast<uint32_t>(wordsVal[j].getInt64()));
        tl_ump_words_storage.push_back(std::move(words));
        auto& stored = tl_ump_words_storage.back();
        tl_ump_events.push_back({ tick, static_cast<uint32_t>(stored.size()), stored.data() });
    }

    return {
        true, nullptr,
        static_cast<uint32_t>(tl_ump_events.size()),
        tl_ump_events.data()
    };
}

bool uapmd_app_add_ump_event_to_clip(uapmd_app_model_t app,
                                       int32_t track_index,
                                       int32_t clip_id,
                                       uint64_t tick,
                                       const uint32_t* words,
                                       uint32_t word_count) {
    tl_ump_error.clear();
    std::vector<uint32_t> w(words, words + word_count);
    return AM(app)->addUmpEventToClip(track_index, clip_id, tick, std::move(w), tl_ump_error);
}

bool uapmd_app_remove_ump_event_from_clip(uapmd_app_model_t app,
                                            int32_t track_index,
                                            int32_t clip_id,
                                            int32_t event_index) {
    tl_ump_error.clear();
    return AM(app)->removeUmpEventFromClip(track_index, clip_id, event_index, tl_ump_error);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Project save/load
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_save_project(uapmd_app_model_t app, const char* file_path, void* user_data, uapmd_project_save_cb_t callback) {
    AM(app)->saveProject(file_path, [callback, user_data](uapmd::AppModel::ProjectResult r) {
        if (!callback) return;
        tl_error = r.error;
        uapmd_app_project_result_t cr = { r.success, tl_error.empty() ? nullptr : tl_error.c_str() };
        callback(cr, user_data);
    });
}

uapmd_app_project_result_t uapmd_app_save_project_sync(uapmd_app_model_t app, const char* file_path) {
    auto r = AM(app)->saveProjectSync(file_path);
    tl_error = r.error;
    return { r.success, tl_error.empty() ? nullptr : tl_error.c_str() };
}

uapmd_app_project_result_t uapmd_app_load_project(uapmd_app_model_t app, const char* file_path) {
    auto r = AM(app)->loadProject(file_path);
    tl_error = r.error;
    return { r.success, tl_error.empty() ? nullptr : tl_error.c_str() };
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Offline rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

bool uapmd_app_start_render(uapmd_app_model_t app, const uapmd_app_render_settings_t* settings) {
    uapmd::AppModel::RenderToFileSettings s;
    if (settings->output_path) s.outputPath = settings->output_path;
    s.startSeconds = settings->start_seconds;
    if (settings->has_end_seconds)
        s.endSeconds = settings->end_seconds;
    s.useContentFallback = settings->use_content_fallback;
    s.contentBoundsValid = settings->content_bounds_valid;
    s.contentStartSeconds = settings->content_start_seconds;
    s.contentEndSeconds = settings->content_end_seconds;
    s.tailSeconds = settings->tail_seconds;
    s.enableSilenceStop = settings->enable_silence_stop;
    s.silenceDurationSeconds = settings->silence_duration_seconds;
    s.silenceThresholdDb = settings->silence_threshold_db;
    return AM(app)->startRenderToFile(s);
}

void uapmd_app_cancel_render(uapmd_app_model_t app) { AM(app)->cancelRenderToFile(); }

static thread_local std::string tl_render_msg;
static thread_local std::string tl_render_path;

uapmd_app_render_status_t uapmd_app_get_render_status(uapmd_app_model_t app) {
    auto st = AM(app)->getRenderToFileStatus();
    tl_render_msg = st.message;
    tl_render_path = st.outputPath.string();
    return {
        st.running, st.completed, st.success, st.progress, st.renderedSeconds,
        tl_render_msg.empty() ? nullptr : tl_render_msg.c_str(),
        tl_render_path.empty() ? nullptr : tl_render_path.c_str()
    };
}

void uapmd_app_clear_render_status(uapmd_app_model_t app) { AM(app)->clearCompletedRenderStatus(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  TransportController
 * ═══════════════════════════════════════════════════════════════════════════ */

bool  uapmd_transport_is_playing(uapmd_transport_controller_t tc)    { return TC(tc)->isPlaying(); }
bool  uapmd_transport_is_paused(uapmd_transport_controller_t tc)     { return TC(tc)->isPaused(); }
bool  uapmd_transport_is_recording(uapmd_transport_controller_t tc)  { return TC(tc)->isRecording(); }
float uapmd_transport_get_volume(uapmd_transport_controller_t tc)    { return TC(tc)->volume(); }
void  uapmd_transport_set_volume(uapmd_transport_controller_t tc, float v) { TC(tc)->setVolume(v); }

void uapmd_transport_play(uapmd_transport_controller_t tc)    { TC(tc)->play(); }
void uapmd_transport_stop(uapmd_transport_controller_t tc)    { TC(tc)->stop(); }
void uapmd_transport_pause(uapmd_transport_controller_t tc)   { TC(tc)->pause(); }
void uapmd_transport_resume(uapmd_transport_controller_t tc)  { TC(tc)->resume(); }
void uapmd_transport_record(uapmd_transport_controller_t tc)  { TC(tc)->record(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  Startup lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_app_notify_ui_ready(uapmd_app_model_t app)                 { AM(app)->notifyUiReady(); }
void uapmd_app_notify_persistent_storage_ready(uapmd_app_model_t app) { AM(app)->notifyPersistentStorageReady(); }
