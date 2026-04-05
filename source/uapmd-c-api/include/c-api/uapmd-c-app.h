/* uapmd C API — bindings for AppModel (uapmd-app) */
#ifndef UAPMD_C_APP_H
#define UAPMD_C_APP_H

#include "uapmd-c-common.h"
#include "uapmd-c-api.h"
#include "uapmd-c-data.h"
#include "uapmd-c-engine.h"
#include "uapmd-c-file.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ───────────────────────────────────────────────────────── */

typedef struct uapmd_app_model*           uapmd_app_model_t;
typedef struct uapmd_transport_controller* uapmd_transport_controller_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void uapmd_app_instantiate(void);
UAPMD_C_EXPORT uapmd_app_model_t uapmd_app_instance(void);
UAPMD_C_EXPORT void uapmd_app_cleanup(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Accessors
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_realtime_sequencer_t uapmd_app_sequencer(uapmd_app_model_t app);
UAPMD_C_EXPORT uapmd_transport_controller_t uapmd_app_transport(uapmd_app_model_t app);
UAPMD_C_EXPORT uapmd_document_provider_t uapmd_app_document_provider(uapmd_app_model_t app);
UAPMD_C_EXPORT int32_t uapmd_app_sample_rate(uapmd_app_model_t app);
UAPMD_C_EXPORT uint32_t uapmd_app_track_count(uapmd_app_model_t app);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Audio engine control
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT bool uapmd_app_is_scanning(uapmd_app_model_t app);
UAPMD_C_EXPORT bool uapmd_app_is_audio_engine_enabled(uapmd_app_model_t app);
UAPMD_C_EXPORT void uapmd_app_set_audio_engine_enabled(uapmd_app_model_t app, bool enabled);
UAPMD_C_EXPORT void uapmd_app_toggle_audio_engine(uapmd_app_model_t app);
UAPMD_C_EXPORT void uapmd_app_update_audio_device_settings(uapmd_app_model_t app, int32_t sample_rate, uint32_t buffer_size);
UAPMD_C_EXPORT void uapmd_app_set_auto_buffer_size_enabled(uapmd_app_model_t app, bool enabled);
UAPMD_C_EXPORT bool uapmd_app_auto_buffer_size_enabled(uapmd_app_model_t app);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin scanning
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum uapmd_plugin_scan_request {
    UAPMD_PLUGIN_SCAN_IN_PROCESS    = 0,
    UAPMD_PLUGIN_SCAN_REMOTE_PROCESS = 1
} uapmd_plugin_scan_request_t;

UAPMD_C_EXPORT void uapmd_app_perform_plugin_scanning(uapmd_app_model_t app,
                                                        bool force_rescan,
                                                        uapmd_plugin_scan_request_t request,
                                                        double remote_timeout_seconds,
                                                        bool require_fast_scanning);
UAPMD_C_EXPORT void uapmd_app_cancel_plugin_scanning(uapmd_app_model_t app);
UAPMD_C_EXPORT size_t uapmd_app_generate_scan_report(uapmd_app_model_t app, char* buf, size_t buf_size);
UAPMD_C_EXPORT void uapmd_app_clear_plugin_blocklist(uapmd_app_model_t app);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin instance management
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_plugin_instance_config {
    const char* api_name;        /* default: "default" */
    const char* device_name;     /* empty = auto-generate */
    const char* manufacturer;    /* default: "UAPMD Project" */
    const char* version;         /* default: "0.1" */
    const char* state_file;      /* path or empty */
} uapmd_plugin_instance_config_t;

typedef struct uapmd_plugin_instance_result {
    int32_t instance_id;
    const char* plugin_name;
    const char* error;
} uapmd_plugin_instance_result_t;

typedef void (*uapmd_instance_created_cb_t)(uapmd_plugin_instance_result_t result, void* user_data);

UAPMD_C_EXPORT void uapmd_app_create_plugin_instance(uapmd_app_model_t app,
                                                       const char* format,
                                                       const char* plugin_id,
                                                       int32_t track_index,
                                                       const uapmd_plugin_instance_config_t* config,
                                                       void* user_data,
                                                       uapmd_instance_created_cb_t callback);

UAPMD_C_EXPORT void uapmd_app_remove_plugin_instance(uapmd_app_model_t app, int32_t instance_id);

/* UMP group */
UAPMD_C_EXPORT uint8_t uapmd_app_get_instance_group(uapmd_app_model_t app, int32_t instance_id);
UAPMD_C_EXPORT bool    uapmd_app_set_instance_group(uapmd_app_model_t app, int32_t instance_id, uint8_t group);

/* UMP device enable/disable */
UAPMD_C_EXPORT void uapmd_app_enable_ump_device(uapmd_app_model_t app, int32_t instance_id, const char* device_name);
UAPMD_C_EXPORT void uapmd_app_disable_ump_device(uapmd_app_model_t app, int32_t instance_id);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Instance details
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void uapmd_app_request_show_instance_details(uapmd_app_model_t app, int32_t instance_id);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin UI
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void uapmd_app_request_show_plugin_ui(uapmd_app_model_t app, int32_t instance_id);
UAPMD_C_EXPORT void uapmd_app_show_plugin_ui(uapmd_app_model_t app,
                                                int32_t instance_id,
                                                bool needs_create,
                                                bool is_floating,
                                                void* parent_handle,
                                                void* resize_user_data,
                                                uapmd_ui_resize_handler_t resize_handler);
UAPMD_C_EXPORT void uapmd_app_hide_plugin_ui(uapmd_app_model_t app, int32_t instance_id);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Plugin state save/load
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_plugin_state_result {
    int32_t instance_id;
    bool success;
    const char* error;
    const char* filepath;
} uapmd_plugin_state_result_t;

typedef void (*uapmd_plugin_state_cb_t)(uapmd_plugin_state_result_t result, void* user_data);

UAPMD_C_EXPORT void uapmd_app_load_plugin_state(uapmd_app_model_t app,
                                                   int32_t instance_id,
                                                   const char* filepath,
                                                   void* user_data,
                                                   uapmd_plugin_state_cb_t callback);
UAPMD_C_EXPORT void uapmd_app_save_plugin_state(uapmd_app_model_t app,
                                                   int32_t instance_id,
                                                   const char* filepath,
                                                   void* user_data,
                                                   uapmd_plugin_state_cb_t callback);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Clip management
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_app_add_clip_to_track(uapmd_app_model_t app,
                                                                      int32_t track_index,
                                                                      uapmd_timeline_position_t position,
                                                                      uapmd_audio_file_reader_t reader,
                                                                      const char* filepath);

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_app_add_midi_clip_to_track(uapmd_app_model_t app,
                                                                           int32_t track_index,
                                                                           uapmd_timeline_position_t position,
                                                                           const char* filepath);

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_app_add_midi_clip_from_data(uapmd_app_model_t app,
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
                                                                            bool needs_file_save);

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_app_create_empty_midi_clip(uapmd_app_model_t app,
                                                                           int32_t track_index,
                                                                           int64_t position_samples,
                                                                           uint32_t tick_resolution,
                                                                           double bpm);

UAPMD_C_EXPORT bool uapmd_app_remove_clip_from_track(uapmd_app_model_t app, int32_t track_index, int32_t clip_id);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Track management
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT int32_t uapmd_app_add_track(uapmd_app_model_t app);
UAPMD_C_EXPORT bool    uapmd_app_remove_track(uapmd_app_model_t app, int32_t track_index);
UAPMD_C_EXPORT void    uapmd_app_remove_all_tracks(uapmd_app_model_t app);

UAPMD_C_EXPORT int32_t uapmd_app_add_device_input_to_track(uapmd_app_model_t app,
                                                              int32_t track_index,
                                                              const uint32_t* channel_indices,
                                                              uint32_t channel_count);

/* Timeline tracks */
UAPMD_C_EXPORT uint32_t uapmd_app_timeline_track_count(uapmd_app_model_t app);
UAPMD_C_EXPORT uapmd_timeline_track_t uapmd_app_get_timeline_track(uapmd_app_model_t app, uint32_t index);
UAPMD_C_EXPORT uapmd_timeline_track_t uapmd_app_master_timeline_track(uapmd_app_model_t app);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Timeline state access
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT bool uapmd_app_get_timeline_state(uapmd_app_model_t app, uapmd_timeline_state_t* out);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Track graph editing (DAG graph)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum uapmd_graph_endpoint_type {
    UAPMD_GRAPH_ENDPOINT_GRAPH_INPUT  = 0,
    UAPMD_GRAPH_ENDPOINT_PLUGIN       = 1,
    UAPMD_GRAPH_ENDPOINT_GRAPH_OUTPUT = 2
} uapmd_graph_endpoint_type_t;

typedef enum uapmd_graph_bus_type {
    UAPMD_GRAPH_BUS_AUDIO = 0,
    UAPMD_GRAPH_BUS_EVENT = 1
} uapmd_graph_bus_type_t;

typedef struct uapmd_graph_endpoint {
    uapmd_graph_endpoint_type_t type;
    int32_t instance_id;
    uint32_t bus_index;
} uapmd_graph_endpoint_t;

typedef struct uapmd_graph_connection {
    int64_t id;
    uapmd_graph_bus_type_t bus_type;
    uapmd_graph_endpoint_t source;
    uapmd_graph_endpoint_t target;
} uapmd_graph_connection_t;

typedef struct uapmd_graph_connections_result {
    bool success;
    const char* error;
    uint32_t count;
    const uapmd_graph_connection_t* connections;
} uapmd_graph_connections_result_t;

typedef struct uapmd_op_result {
    bool success;
    const char* error;
} uapmd_op_result_t;

UAPMD_C_EXPORT bool uapmd_app_ensure_track_uses_editor_graph(uapmd_app_model_t app, int32_t track_index);
UAPMD_C_EXPORT void uapmd_app_request_show_track_graph(uapmd_app_model_t app, int32_t track_index);
UAPMD_C_EXPORT bool uapmd_app_revert_track_to_simple_graph(uapmd_app_model_t app, int32_t track_index);

UAPMD_C_EXPORT uapmd_graph_connections_result_t uapmd_app_get_track_graph_connections(uapmd_app_model_t app, int32_t track_index);
UAPMD_C_EXPORT uapmd_op_result_t uapmd_app_connect_track_graph(uapmd_app_model_t app,
                                                                  int32_t track_index,
                                                                  const uapmd_graph_connection_t* connection);
UAPMD_C_EXPORT uapmd_op_result_t uapmd_app_disconnect_track_graph_connection(uapmd_app_model_t app,
                                                                               int32_t track_index,
                                                                               int64_t connection_id);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Clip audio events (markers + warps)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_clip_audio_events_result {
    bool success;
    const char* error;
    uint32_t marker_count;
    const uapmd_clip_marker_t* markers;
    uint32_t audio_warp_count;
    const uapmd_audio_warp_point_t* audio_warps;
} uapmd_clip_audio_events_result_t;

UAPMD_C_EXPORT uapmd_clip_audio_events_result_t uapmd_app_get_clip_audio_events(uapmd_app_model_t app,
                                                                                   int32_t track_index,
                                                                                   int32_t clip_id);
UAPMD_C_EXPORT uapmd_op_result_t uapmd_app_set_clip_audio_events(uapmd_app_model_t app,
                                                                     int32_t track_index,
                                                                     int32_t clip_id,
                                                                     const uapmd_clip_marker_t* markers,
                                                                     uint32_t marker_count,
                                                                     const uapmd_audio_warp_point_t* warps,
                                                                     uint32_t warp_count);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Master track markers
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uint32_t uapmd_app_master_marker_count(uapmd_app_model_t app);
UAPMD_C_EXPORT bool     uapmd_app_get_master_marker(uapmd_app_model_t app, uint32_t index, uapmd_clip_marker_t* out);
UAPMD_C_EXPORT uapmd_op_result_t uapmd_app_set_master_markers(uapmd_app_model_t app,
                                                                 const uapmd_clip_marker_t* markers,
                                                                 uint32_t count);

/* ═══════════════════════════════════════════════════════════════════════════
 *  MIDI clip UMP event editing
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_ump_event {
    uint64_t tick;
    uint32_t word_count;
    const uint32_t* words;
} uapmd_ump_event_t;

typedef struct uapmd_ump_events_result {
    bool success;
    const char* error;
    uint32_t event_count;
    const uapmd_ump_event_t* events;
} uapmd_ump_events_result_t;

UAPMD_C_EXPORT uapmd_ump_events_result_t uapmd_app_get_midi_clip_ump_events(uapmd_app_model_t app,
                                                                               int32_t track_index,
                                                                               int32_t clip_id);
UAPMD_C_EXPORT bool uapmd_app_add_ump_event_to_clip(uapmd_app_model_t app,
                                                       int32_t track_index,
                                                       int32_t clip_id,
                                                       uint64_t tick,
                                                       const uint32_t* words,
                                                       uint32_t word_count);
UAPMD_C_EXPORT bool uapmd_app_remove_ump_event_from_clip(uapmd_app_model_t app,
                                                            int32_t track_index,
                                                            int32_t clip_id,
                                                            int32_t event_index);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Project save/load
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_app_project_result {
    bool success;
    const char* error;
} uapmd_app_project_result_t;

typedef void (*uapmd_project_save_cb_t)(uapmd_app_project_result_t result, void* user_data);

UAPMD_C_EXPORT void uapmd_app_save_project(uapmd_app_model_t app, const char* file_path, void* user_data, uapmd_project_save_cb_t callback);
UAPMD_C_EXPORT uapmd_app_project_result_t uapmd_app_save_project_sync(uapmd_app_model_t app, const char* file_path);
UAPMD_C_EXPORT uapmd_app_project_result_t uapmd_app_load_project(uapmd_app_model_t app, const char* file_path);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Offline rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_app_render_settings {
    const char* output_path;
    double start_seconds;
    double end_seconds;             /* 0 if not set */
    bool has_end_seconds;
    bool use_content_fallback;
    bool content_bounds_valid;
    double content_start_seconds;
    double content_end_seconds;
    double tail_seconds;
    bool enable_silence_stop;
    double silence_duration_seconds;
    double silence_threshold_db;
} uapmd_app_render_settings_t;

typedef struct uapmd_app_render_status {
    bool running;
    bool completed;
    bool success;
    double progress;
    double rendered_seconds;
    const char* message;
    const char* output_path;
} uapmd_app_render_status_t;

UAPMD_C_EXPORT bool uapmd_app_start_render(uapmd_app_model_t app, const uapmd_app_render_settings_t* settings);
UAPMD_C_EXPORT void uapmd_app_cancel_render(uapmd_app_model_t app);
UAPMD_C_EXPORT uapmd_app_render_status_t uapmd_app_get_render_status(uapmd_app_model_t app);
UAPMD_C_EXPORT void uapmd_app_clear_render_status(uapmd_app_model_t app);

/* ═══════════════════════════════════════════════════════════════════════════
 *  TransportController
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT bool  uapmd_transport_is_playing(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT bool  uapmd_transport_is_paused(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT bool  uapmd_transport_is_recording(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT float uapmd_transport_get_volume(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT void  uapmd_transport_set_volume(uapmd_transport_controller_t tc, float volume);

UAPMD_C_EXPORT void uapmd_transport_play(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT void uapmd_transport_stop(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT void uapmd_transport_pause(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT void uapmd_transport_resume(uapmd_transport_controller_t tc);
UAPMD_C_EXPORT void uapmd_transport_record(uapmd_transport_controller_t tc);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Startup lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void uapmd_app_notify_ui_ready(uapmd_app_model_t app);
UAPMD_C_EXPORT void uapmd_app_notify_persistent_storage_ready(uapmd_app_model_t app);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_APP_H */
