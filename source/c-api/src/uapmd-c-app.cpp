/* uapmd C API — implementation for AppModel bindings */

#include "c-api/uapmd-c-app.h"
#include "c-api-internal.h"
#include "AppModel.hpp"
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
