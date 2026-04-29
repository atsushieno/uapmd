/* uapmd C API — bindings for the uapmd-engine module */
#ifndef UAPMD_C_ENGINE_H
#define UAPMD_C_ENGINE_H

#include "uapmd-c-common.h"
#include "uapmd-c-data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handles (uapmd-engine) ───────────────────────────────────────── */

typedef struct uapmd_sequencer_engine*   uapmd_sequencer_engine_t;
typedef struct uapmd_sequencer_track*    uapmd_sequencer_track_t;
typedef struct uapmd_timeline_facade*    uapmd_timeline_facade_t;
typedef struct uapmd_audio_io_device*    uapmd_audio_io_device_t;
typedef struct uapmd_audio_io_device_mgr* uapmd_audio_io_device_mgr_t;
typedef struct uapmd_midi_io_device*     uapmd_midi_io_device_t;
typedef struct uapmd_device_io_dispatcher* uapmd_device_io_dispatcher_t;
typedef struct uapmd_realtime_sequencer* uapmd_realtime_sequencer_t;

/* ── Constants ───────────────────────────────────────────────────────────── */

#define UAPMD_MASTER_TRACK_INDEX INT32_MIN

/* ═══════════════════════════════════════════════════════════════════════════
 *  Audio device enums and structs
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum uapmd_audio_io_direction {
    UAPMD_AUDIO_DIRECTION_INPUT  = 1,
    UAPMD_AUDIO_DIRECTION_OUTPUT = 2,
    UAPMD_AUDIO_DIRECTION_DUPLEX = 3
} uapmd_audio_io_direction_t;

typedef enum uapmd_audio_device_change {
    UAPMD_AUDIO_DEVICE_CHANGE_ADDED   = 1,
    UAPMD_AUDIO_DEVICE_CHANGE_REMOVED = 2
} uapmd_audio_device_change_t;

typedef struct uapmd_audio_device_info {
    uapmd_audio_io_direction_t directions;
    int32_t id;
    const char* name;
    uint32_t sample_rate;
    uint32_t channels;
} uapmd_audio_device_info_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  SequencerEngine
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_sequencer_engine_t uapmd_engine_create(int32_t sample_rate,
                                                              uint32_t audio_buffer_size,
                                                              uint32_t ump_buffer_size);
UAPMD_C_EXPORT void uapmd_engine_destroy(uapmd_sequencer_engine_t engine);

UAPMD_C_EXPORT void uapmd_engine_enqueue_ump(uapmd_sequencer_engine_t engine,
                                               int32_t instance_id,
                                               uapmd_ump_t* ump,
                                               size_t size_in_bytes,
                                               uapmd_timestamp_t timestamp);

UAPMD_C_EXPORT uapmd_plugin_host_t uapmd_engine_plugin_host(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT uapmd_plugin_instance_t uapmd_engine_get_plugin_instance(uapmd_sequencer_engine_t engine, int32_t instance_id);
UAPMD_C_EXPORT uapmd_function_block_mgr_t uapmd_engine_function_block_manager(uapmd_sequencer_engine_t engine);

/* Track management */
UAPMD_C_EXPORT uint32_t uapmd_engine_track_count(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT uapmd_sequencer_track_t uapmd_engine_get_track(uapmd_sequencer_engine_t engine, uint32_t index);
UAPMD_C_EXPORT uapmd_sequencer_track_t uapmd_engine_master_track(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT int32_t  uapmd_engine_add_empty_track(uapmd_sequencer_engine_t engine);

typedef void (*uapmd_add_plugin_cb_t)(int32_t instance_id, int32_t track_index, const char* error, void* user_data);

UAPMD_C_EXPORT void uapmd_engine_add_plugin_to_track(uapmd_sequencer_engine_t engine,
                                                       int32_t track_index,
                                                       const char* format,
                                                       const char* plugin_id,
                                                       void* user_data,
                                                       uapmd_add_plugin_cb_t callback);

UAPMD_C_EXPORT bool uapmd_engine_remove_plugin_instance(uapmd_sequencer_engine_t engine, int32_t instance_id);
UAPMD_C_EXPORT bool uapmd_engine_remove_track(uapmd_sequencer_engine_t engine, int32_t track_index);
UAPMD_C_EXPORT void uapmd_engine_cleanup_empty_tracks(uapmd_sequencer_engine_t engine);

UAPMD_C_EXPORT int32_t uapmd_engine_find_track_for_instance(uapmd_sequencer_engine_t engine, int32_t instance_id);
UAPMD_C_EXPORT uint8_t uapmd_engine_get_instance_group(uapmd_sequencer_engine_t engine, int32_t instance_id);
UAPMD_C_EXPORT bool    uapmd_engine_set_instance_group(uapmd_sequencer_engine_t engine, int32_t instance_id, uint8_t group);

/* Latency */
UAPMD_C_EXPORT uint32_t uapmd_engine_track_latency(uapmd_sequencer_engine_t engine, int32_t track_index);
UAPMD_C_EXPORT uint32_t uapmd_engine_master_track_latency(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT uint32_t uapmd_engine_track_render_lead(uapmd_sequencer_engine_t engine, int32_t track_index);
UAPMD_C_EXPORT uint32_t uapmd_engine_master_track_render_lead(uapmd_sequencer_engine_t engine);

/* Configuration */
UAPMD_C_EXPORT void uapmd_engine_set_default_channels(uapmd_sequencer_engine_t engine, uint32_t input_channels, uint32_t output_channels);
UAPMD_C_EXPORT void uapmd_engine_set_sample_rate(uapmd_sequencer_engine_t engine, int32_t sample_rate);
UAPMD_C_EXPORT bool uapmd_engine_get_offline_rendering(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT void uapmd_engine_set_offline_rendering(uapmd_sequencer_engine_t engine, bool enabled);
UAPMD_C_EXPORT void uapmd_engine_set_active(uapmd_sequencer_engine_t engine, bool active);
UAPMD_C_EXPORT void uapmd_engine_set_external_pump(uapmd_sequencer_engine_t engine, bool enabled);

/* Playback */
UAPMD_C_EXPORT bool    uapmd_engine_is_playback_active(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT int64_t uapmd_engine_get_playback_position(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT void    uapmd_engine_set_playback_position(uapmd_sequencer_engine_t engine, int64_t samples);
UAPMD_C_EXPORT int64_t uapmd_engine_render_playback_position(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT void    uapmd_engine_start_playback(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT void    uapmd_engine_stop_playback(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT void    uapmd_engine_pause_playback(uapmd_sequencer_engine_t engine);
UAPMD_C_EXPORT void    uapmd_engine_resume_playback(uapmd_sequencer_engine_t engine);

/* Convenience MIDI */
UAPMD_C_EXPORT void uapmd_engine_send_note_on(uapmd_sequencer_engine_t engine, int32_t instance_id, int32_t note);
UAPMD_C_EXPORT void uapmd_engine_send_note_off(uapmd_sequencer_engine_t engine, int32_t instance_id, int32_t note);
UAPMD_C_EXPORT void uapmd_engine_send_pitch_bend(uapmd_sequencer_engine_t engine, int32_t instance_id, float value);
UAPMD_C_EXPORT void uapmd_engine_send_channel_pressure(uapmd_sequencer_engine_t engine, int32_t instance_id, float pressure);
UAPMD_C_EXPORT void uapmd_engine_set_parameter_value(uapmd_sequencer_engine_t engine, int32_t instance_id, int32_t index, double value);

/* Audio analysis */
UAPMD_C_EXPORT void uapmd_engine_get_input_spectrum(uapmd_sequencer_engine_t engine, float* out_spectrum, int num_bars);
UAPMD_C_EXPORT void uapmd_engine_get_output_spectrum(uapmd_sequencer_engine_t engine, float* out_spectrum, int num_bars);

/* Timeline facade access */
UAPMD_C_EXPORT uapmd_timeline_facade_t uapmd_engine_timeline(uapmd_sequencer_engine_t engine);

/* ═══════════════════════════════════════════════════════════════════════════
 *  SequencerTrack
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_plugin_graph_t uapmd_track_graph(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT uint32_t uapmd_track_latency_in_samples(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT uint32_t uapmd_track_render_lead_in_samples(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT double   uapmd_track_tail_length_in_seconds(uapmd_sequencer_track_t track);

UAPMD_C_EXPORT bool uapmd_track_get_bypassed(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT bool uapmd_track_get_frozen(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT void uapmd_track_set_bypassed(uapmd_sequencer_track_t track, bool value);
UAPMD_C_EXPORT void uapmd_track_set_frozen(uapmd_sequencer_track_t track, bool value);

UAPMD_C_EXPORT uint32_t uapmd_track_ordered_instance_id_count(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT bool     uapmd_track_get_ordered_instance_ids(uapmd_sequencer_track_t track, int32_t* out, uint32_t out_count);

UAPMD_C_EXPORT void    uapmd_track_set_instance_group(uapmd_sequencer_track_t track, int32_t instance_id, uint8_t group);
UAPMD_C_EXPORT uint8_t uapmd_track_get_instance_group(uapmd_sequencer_track_t track, int32_t instance_id);
UAPMD_C_EXPORT uint8_t uapmd_track_find_available_group(uapmd_sequencer_track_t track);
UAPMD_C_EXPORT void    uapmd_track_remove_instance(uapmd_sequencer_track_t track, int32_t instance_id);

/* ═══════════════════════════════════════════════════════════════════════════
 *  TimelineFacade
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT bool uapmd_tl_get_state(uapmd_timeline_facade_t tl, uapmd_timeline_state_t* out);
UAPMD_C_EXPORT void uapmd_tl_set_tempo(uapmd_timeline_facade_t tl, double tempo);
UAPMD_C_EXPORT void uapmd_tl_set_time_signature(uapmd_timeline_facade_t tl, int32_t numerator, int32_t denominator);
UAPMD_C_EXPORT void uapmd_tl_set_loop(uapmd_timeline_facade_t tl, bool enabled, uapmd_timeline_position_t start, uapmd_timeline_position_t end);

UAPMD_C_EXPORT uint32_t uapmd_tl_track_count(uapmd_timeline_facade_t tl);
UAPMD_C_EXPORT uapmd_timeline_track_t uapmd_tl_get_track(uapmd_timeline_facade_t tl, uint32_t index);
UAPMD_C_EXPORT uapmd_timeline_track_t uapmd_tl_master_timeline_track(uapmd_timeline_facade_t tl);

/* Clip add results */
typedef struct uapmd_clip_add_result {
    int32_t clip_id;
    int32_t source_node_id;
    bool success;
    const char* error;
} uapmd_clip_add_result_t;

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_tl_add_audio_clip(uapmd_timeline_facade_t tl,
                                                                  int32_t track_index,
                                                                  uapmd_timeline_position_t position,
                                                                  uapmd_audio_file_reader_t reader,
                                                                  const char* filepath);

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_tl_add_midi_clip_from_file(uapmd_timeline_facade_t tl,
                                                                          int32_t track_index,
                                                                          uapmd_timeline_position_t position,
                                                                          const char* filepath,
                                                                          bool nrpn_to_parameter_mapping);

UAPMD_C_EXPORT uapmd_clip_add_result_t uapmd_tl_add_midi_clip_from_data(uapmd_timeline_facade_t tl,
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
                                                                          bool nrpn_to_parameter_mapping,
                                                                          bool needs_file_save);

UAPMD_C_EXPORT bool uapmd_tl_remove_clip(uapmd_timeline_facade_t tl, int32_t track_index, int32_t clip_id);

/* Project loading */
typedef struct uapmd_project_result {
    bool success;
    const char* error;
} uapmd_project_result_t;

UAPMD_C_EXPORT uapmd_project_result_t uapmd_tl_load_project(uapmd_timeline_facade_t tl, const char* file_path);

/* Content bounds */
typedef struct uapmd_content_bounds {
    bool has_content;
    int64_t first_sample;
    int64_t last_sample;
    double first_seconds;
    double last_seconds;
} uapmd_content_bounds_t;

UAPMD_C_EXPORT uapmd_content_bounds_t uapmd_tl_calculate_content_bounds(uapmd_timeline_facade_t tl);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioIODeviceManager
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_audio_io_device_mgr_t uapmd_audio_device_mgr_instance(const char* driver_name);

UAPMD_C_EXPORT uint32_t uapmd_audio_device_mgr_device_count(uapmd_audio_io_device_mgr_t mgr);
UAPMD_C_EXPORT bool     uapmd_audio_device_mgr_get_device_info(uapmd_audio_io_device_mgr_t mgr, uint32_t index, uapmd_audio_device_info_t* out);

UAPMD_C_EXPORT uapmd_audio_io_device_t uapmd_audio_device_mgr_open(uapmd_audio_io_device_mgr_t mgr,
                                                                      int input_device_index,
                                                                      int output_device_index,
                                                                      uint32_t sample_rate,
                                                                      uint32_t buffer_size);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioIODevice
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT double   uapmd_audio_device_sample_rate(uapmd_audio_io_device_t dev);
UAPMD_C_EXPORT uint32_t uapmd_audio_device_channels(uapmd_audio_io_device_t dev);
UAPMD_C_EXPORT uint32_t uapmd_audio_device_input_channels(uapmd_audio_io_device_t dev);
UAPMD_C_EXPORT uint32_t uapmd_audio_device_output_channels(uapmd_audio_io_device_t dev);
UAPMD_C_EXPORT uapmd_status_t uapmd_audio_device_start(uapmd_audio_io_device_t dev);
UAPMD_C_EXPORT uapmd_status_t uapmd_audio_device_stop(uapmd_audio_io_device_t dev);
UAPMD_C_EXPORT bool     uapmd_audio_device_is_playing(uapmd_audio_io_device_t dev);

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiIODevice
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_midi_io_device_t uapmd_midi_device_instance(const char* driver_name);

/* ═══════════════════════════════════════════════════════════════════════════
 *  DeviceIODispatcher
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_device_io_dispatcher_t uapmd_default_device_io_dispatcher(void);
UAPMD_C_EXPORT uapmd_status_t uapmd_dispatcher_start(uapmd_device_io_dispatcher_t disp);
UAPMD_C_EXPORT uapmd_status_t uapmd_dispatcher_stop(uapmd_device_io_dispatcher_t disp);
UAPMD_C_EXPORT bool uapmd_dispatcher_is_playing(uapmd_device_io_dispatcher_t disp);
UAPMD_C_EXPORT void uapmd_dispatcher_clear_output_buffers(uapmd_device_io_dispatcher_t disp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  RealtimeSequencer
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_realtime_sequencer_t uapmd_rt_sequencer_create(uint32_t buffer_size,
                                                                       uint32_t ump_buffer_size,
                                                                       int32_t sample_rate,
                                                                       uapmd_device_io_dispatcher_t dispatcher);
UAPMD_C_EXPORT void uapmd_rt_sequencer_destroy(uapmd_realtime_sequencer_t seq);

UAPMD_C_EXPORT uapmd_sequencer_engine_t uapmd_rt_sequencer_engine(uapmd_realtime_sequencer_t seq);
UAPMD_C_EXPORT uapmd_status_t uapmd_rt_sequencer_start_audio(uapmd_realtime_sequencer_t seq);
UAPMD_C_EXPORT uapmd_status_t uapmd_rt_sequencer_stop_audio(uapmd_realtime_sequencer_t seq);
UAPMD_C_EXPORT uapmd_status_t uapmd_rt_sequencer_is_audio_playing(uapmd_realtime_sequencer_t seq);
UAPMD_C_EXPORT void uapmd_rt_sequencer_clear_output_buffers(uapmd_realtime_sequencer_t seq);
UAPMD_C_EXPORT int32_t uapmd_rt_sequencer_sample_rate(uapmd_realtime_sequencer_t seq);
UAPMD_C_EXPORT bool    uapmd_rt_sequencer_set_sample_rate(uapmd_realtime_sequencer_t seq, int32_t new_sample_rate);
UAPMD_C_EXPORT bool uapmd_rt_sequencer_reconfigure_audio_device(uapmd_realtime_sequencer_t seq,
                                                                   int input_device_index,
                                                                   int output_device_index,
                                                                   uint32_t sample_rate,
                                                                   uint32_t buffer_size);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Offline Renderer
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_offline_render_settings {
    const char* output_path;
    double start_seconds;
    double end_seconds;         /* 0 for unset */
    bool has_end_seconds;
    bool use_content_fallback;
    bool content_bounds_valid;
    double content_start_seconds;
    double content_end_seconds;
    double tail_seconds;
    bool enable_silence_stop;
    double silence_duration_seconds;
    double silence_threshold_db;
    int32_t sample_rate;
    uint32_t buffer_size;
    uint32_t output_channels;
    uint32_t ump_buffer_size;
} uapmd_offline_render_settings_t;

typedef struct uapmd_offline_render_progress {
    double progress;
    double rendered_seconds;
    double total_seconds;
    int64_t rendered_frames;
    int64_t total_frames;
} uapmd_offline_render_progress_t;

typedef void (*uapmd_render_progress_cb_t)(const uapmd_offline_render_progress_t* progress, void* user_data);
typedef bool (*uapmd_render_should_cancel_cb_t)(void* user_data);

typedef struct uapmd_offline_render_result {
    bool success;
    bool canceled;
    double rendered_seconds;
    const char* error_message;
} uapmd_offline_render_result_t;

UAPMD_C_EXPORT uapmd_offline_render_result_t uapmd_render_offline(uapmd_sequencer_engine_t engine,
                                                                     const uapmd_offline_render_settings_t* settings,
                                                                     void* user_data,
                                                                     uapmd_render_progress_cb_t progress_cb,
                                                                     uapmd_render_should_cancel_cb_t cancel_cb);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_ENGINE_H */
