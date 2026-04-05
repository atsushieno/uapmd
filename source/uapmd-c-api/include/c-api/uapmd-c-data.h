/* uapmd C API — bindings for the uapmd-data module */
#ifndef UAPMD_C_DATA_H
#define UAPMD_C_DATA_H

#include "uapmd-c-common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handles (uapmd-data) ─────────────────────────────────────────── */

typedef struct uapmd_clip_manager*          uapmd_clip_manager_t;
typedef struct uapmd_timeline_track*        uapmd_timeline_track_t;
typedef struct uapmd_source_node*           uapmd_source_node_t;
typedef struct uapmd_audio_source_node*     uapmd_audio_source_node_t;
typedef struct uapmd_audio_file_source*     uapmd_audio_file_source_t;
typedef struct uapmd_midi_source_node*      uapmd_midi_source_node_t;
typedef struct uapmd_midi_clip_source*      uapmd_midi_clip_source_t;
typedef struct uapmd_device_input_source*   uapmd_device_input_source_t;
typedef struct uapmd_audio_file_reader*     uapmd_audio_file_reader_t;
typedef struct uapmd_project_data*          uapmd_project_data_t;
typedef struct uapmd_project_track_data*    uapmd_project_track_data_t;
typedef struct uapmd_project_clip_data*     uapmd_project_clip_data_t;
typedef struct uapmd_project_graph_data*    uapmd_project_graph_data_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Enums
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum uapmd_anchor_origin {
    UAPMD_ANCHOR_ORIGIN_START = 0,
    UAPMD_ANCHOR_ORIGIN_END   = 1
} uapmd_anchor_origin_t;

typedef enum uapmd_clip_type {
    UAPMD_CLIP_TYPE_AUDIO = 0,
    UAPMD_CLIP_TYPE_MIDI  = 1
} uapmd_clip_type_t;

typedef enum uapmd_time_reference_type {
    UAPMD_TIME_REF_CONTAINER_START = 0,
    UAPMD_TIME_REF_CONTAINER_END   = 1,
    UAPMD_TIME_REF_POINT           = 2
} uapmd_time_reference_type_t;

typedef enum uapmd_audio_warp_reference_type {
    UAPMD_WARP_REF_MANUAL       = 0,
    UAPMD_WARP_REF_CLIP_START   = 1,
    UAPMD_WARP_REF_CLIP_END     = 2,
    UAPMD_WARP_REF_CLIP_MARKER  = 3,
    UAPMD_WARP_REF_MASTER_MARKER = 4
} uapmd_audio_warp_reference_type_t;

typedef enum uapmd_source_node_type {
    UAPMD_SOURCE_NODE_AUDIO_FILE  = 0,
    UAPMD_SOURCE_NODE_DEVICE_INPUT = 1,
    UAPMD_SOURCE_NODE_MIDI_CLIP   = 2,
    UAPMD_SOURCE_NODE_GENERATOR   = 3
} uapmd_source_node_type_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Timeline data structs
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_timeline_position {
    int64_t samples;
    double legacy_beats;
} uapmd_timeline_position_t;

typedef struct uapmd_time_reference {
    uapmd_time_reference_type_t type;
    const char* reference_id;
    double offset;
} uapmd_time_reference_t;

typedef struct uapmd_clip_marker {
    const char* marker_id;
    double clip_position_offset;
    uapmd_audio_warp_reference_type_t reference_type;
    const char* reference_clip_id;
    const char* reference_marker_id;
    const char* name;
} uapmd_clip_marker_t;

typedef struct uapmd_audio_warp_point {
    double clip_position_offset;
    double speed_ratio;
    uapmd_audio_warp_reference_type_t reference_type;
    const char* reference_clip_id;
    const char* reference_marker_id;
} uapmd_audio_warp_point_t;

typedef struct uapmd_clip_data {
    int32_t clip_id;
    const char* reference_id;
    uapmd_timeline_position_t position;
    int64_t duration_samples;
    int32_t source_node_instance_id;
    double gain;
    bool muted;
    const char* name;
    const char* filepath;
    bool needs_file_save;
    uapmd_clip_type_t clip_type;
    uint32_t tick_resolution;
    double clip_tempo;
    bool nrpn_to_parameter_mapping;
    /* Anchor */
    const char* anchor_reference_id;
    uapmd_anchor_origin_t anchor_origin;
    uapmd_timeline_position_t anchor_offset;
    /* Markers and warps — count + pointer pairs.
     * Only valid until the next mutation on the same clip. */
    uint32_t marker_count;
    const uapmd_clip_marker_t* markers;
    uint32_t audio_warp_count;
    const uapmd_audio_warp_point_t* audio_warps;
} uapmd_clip_data_t;

typedef struct uapmd_timeline_state {
    uapmd_timeline_position_t playhead_position;
    bool is_playing;
    bool loop_enabled;
    uapmd_timeline_position_t loop_start;
    uapmd_timeline_position_t loop_end;
    double tempo;
    int32_t time_signature_numerator;
    int32_t time_signature_denominator;
    int32_t sample_rate;
} uapmd_timeline_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  TimelinePosition helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_timeline_position_t uapmd_position_from_samples(int64_t samples, int32_t sample_rate, double tempo);
UAPMD_C_EXPORT uapmd_timeline_position_t uapmd_position_from_beats(double beats, int32_t sample_rate, double tempo);
UAPMD_C_EXPORT uapmd_timeline_position_t uapmd_position_from_seconds(double seconds, int32_t sample_rate, double tempo);
UAPMD_C_EXPORT double uapmd_position_to_seconds(uapmd_timeline_position_t pos, int32_t sample_rate);

/* ═══════════════════════════════════════════════════════════════════════════
 *  ClipManager
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT int32_t  uapmd_cm_add_clip(uapmd_clip_manager_t cm, const uapmd_clip_data_t* clip);
UAPMD_C_EXPORT bool     uapmd_cm_remove_clip(uapmd_clip_manager_t cm, int32_t clip_id);
UAPMD_C_EXPORT bool     uapmd_cm_get_clip(uapmd_clip_manager_t cm, int32_t clip_id, uapmd_clip_data_t* out);
UAPMD_C_EXPORT uint32_t uapmd_cm_get_all_clips(uapmd_clip_manager_t cm, uapmd_clip_data_t* out, uint32_t out_count);
UAPMD_C_EXPORT size_t   uapmd_cm_clip_count(uapmd_clip_manager_t cm);
UAPMD_C_EXPORT void     uapmd_cm_clear_all(uapmd_clip_manager_t cm);

UAPMD_C_EXPORT bool uapmd_cm_move_clip(uapmd_clip_manager_t cm, int32_t clip_id, uapmd_timeline_position_t new_position);
UAPMD_C_EXPORT bool uapmd_cm_resize_clip(uapmd_clip_manager_t cm, int32_t clip_id, int64_t new_duration);
UAPMD_C_EXPORT bool uapmd_cm_set_clip_gain(uapmd_clip_manager_t cm, int32_t clip_id, double gain);
UAPMD_C_EXPORT bool uapmd_cm_set_clip_muted(uapmd_clip_manager_t cm, int32_t clip_id, bool muted);
UAPMD_C_EXPORT bool uapmd_cm_set_clip_name(uapmd_clip_manager_t cm, int32_t clip_id, const char* name);
UAPMD_C_EXPORT bool uapmd_cm_set_clip_filepath(uapmd_clip_manager_t cm, int32_t clip_id, const char* filepath);
UAPMD_C_EXPORT bool uapmd_cm_set_clip_anchor(uapmd_clip_manager_t cm, int32_t clip_id,
                                               uapmd_time_reference_t anchor, int32_t sample_rate);
UAPMD_C_EXPORT bool uapmd_cm_set_clip_markers(uapmd_clip_manager_t cm, int32_t clip_id,
                                                const uapmd_clip_marker_t* markers, uint32_t count);
UAPMD_C_EXPORT bool uapmd_cm_set_audio_warps(uapmd_clip_manager_t cm, int32_t clip_id,
                                               const uapmd_audio_warp_point_t* warps, uint32_t count);

/* ═══════════════════════════════════════════════════════════════════════════
 *  SourceNode (base)
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT int32_t                 uapmd_sn_instance_id(uapmd_source_node_t sn);
UAPMD_C_EXPORT uapmd_source_node_type_t uapmd_sn_node_type(uapmd_source_node_t sn);
UAPMD_C_EXPORT bool   uapmd_sn_get_disabled(uapmd_source_node_t sn);
UAPMD_C_EXPORT void   uapmd_sn_set_disabled(uapmd_source_node_t sn, bool value);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void    uapmd_asn_seek(uapmd_audio_source_node_t asn, int64_t sample_position);
UAPMD_C_EXPORT int64_t uapmd_asn_current_position(uapmd_audio_source_node_t asn);
UAPMD_C_EXPORT int64_t uapmd_asn_total_length(uapmd_audio_source_node_t asn);
UAPMD_C_EXPORT bool    uapmd_asn_is_playing(uapmd_audio_source_node_t asn);
UAPMD_C_EXPORT void    uapmd_asn_set_playing(uapmd_audio_source_node_t asn, bool playing);
UAPMD_C_EXPORT void    uapmd_asn_process_audio(uapmd_audio_source_node_t asn, float** buffers, uint32_t num_channels, int32_t frame_count);
UAPMD_C_EXPORT uint32_t uapmd_asn_channel_count(uapmd_audio_source_node_t asn);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioFileSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT double  uapmd_afsn_sample_rate(uapmd_audio_file_source_t afsn);
UAPMD_C_EXPORT int64_t uapmd_afsn_num_frames(uapmd_audio_file_source_t afsn);
UAPMD_C_EXPORT uint32_t uapmd_afsn_audio_warp_count(uapmd_audio_file_source_t afsn);
UAPMD_C_EXPORT bool     uapmd_afsn_get_audio_warp(uapmd_audio_file_source_t afsn, uint32_t index, uapmd_audio_warp_point_t* out);

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiClipSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void    uapmd_mcsn_seek(uapmd_midi_clip_source_t mcsn, int64_t sample_position);
UAPMD_C_EXPORT int64_t uapmd_mcsn_current_position(uapmd_midi_clip_source_t mcsn);
UAPMD_C_EXPORT int64_t uapmd_mcsn_total_length(uapmd_midi_clip_source_t mcsn);
UAPMD_C_EXPORT bool    uapmd_mcsn_is_playing(uapmd_midi_clip_source_t mcsn);
UAPMD_C_EXPORT void    uapmd_mcsn_set_playing(uapmd_midi_clip_source_t mcsn, bool playing);
UAPMD_C_EXPORT uint32_t uapmd_mcsn_tick_resolution(uapmd_midi_clip_source_t mcsn);
UAPMD_C_EXPORT double   uapmd_mcsn_clip_tempo(uapmd_midi_clip_source_t mcsn);
UAPMD_C_EXPORT uint32_t uapmd_mcsn_ump_event_count(uapmd_midi_clip_source_t mcsn);
UAPMD_C_EXPORT const uapmd_ump_t* uapmd_mcsn_ump_events(uapmd_midi_clip_source_t mcsn);

/* ═══════════════════════════════════════════════════════════════════════════
 *  DeviceInputSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void uapmd_disn_set_device_input_buffers(uapmd_device_input_source_t disn, float** device_buffers, uint32_t device_channel_count);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioFileReader / AudioFileFactory
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_audio_file_properties {
    uint64_t num_frames;
    uint32_t num_channels;
    uint32_t sample_rate;
} uapmd_audio_file_properties_t;

UAPMD_C_EXPORT uapmd_audio_file_reader_t uapmd_audio_file_reader_create(const char* filepath);
UAPMD_C_EXPORT void uapmd_audio_file_reader_destroy(uapmd_audio_file_reader_t reader);
UAPMD_C_EXPORT bool uapmd_audio_file_reader_get_properties(uapmd_audio_file_reader_t reader, uapmd_audio_file_properties_t* out);
UAPMD_C_EXPORT void uapmd_audio_file_reader_read_frames(uapmd_audio_file_reader_t reader,
                                                          uint64_t start_frame,
                                                          uint64_t frames_to_read,
                                                          float* const* dest,
                                                          uint32_t num_channels);

/* ═══════════════════════════════════════════════════════════════════════════
 *  SmfConverter
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_midi_tempo_change {
    uint64_t tick_position;
    double bpm;
} uapmd_midi_tempo_change_t;

typedef struct uapmd_midi_time_sig_change {
    uint64_t tick_position;
    uint8_t numerator;
    uint8_t denominator;
    uint8_t clocks_per_click;
    uint8_t thirty_seconds_per_quarter;
} uapmd_midi_time_sig_change_t;

typedef struct uapmd_smf_convert_result {
    bool success;
    const char* error;            /* valid until result is freed */
    const uapmd_ump_t* ump_events;
    uint32_t ump_event_count;
    const uint64_t* ump_event_tick_stamps;
    uint32_t tick_stamp_count;
    const uapmd_midi_tempo_change_t* tempo_changes;
    uint32_t tempo_change_count;
    const uapmd_midi_time_sig_change_t* time_sig_changes;
    uint32_t time_sig_change_count;
    uint32_t tick_resolution;
    double detected_tempo;
} uapmd_smf_convert_result_t;

UAPMD_C_EXPORT uapmd_smf_convert_result_t* uapmd_smf_convert_to_ump(const char* smf_file_path);
UAPMD_C_EXPORT uapmd_smf_convert_result_t* uapmd_smf_convert_track_to_ump(const char* smf_file_path, uint32_t track_index);
UAPMD_C_EXPORT void uapmd_smf_convert_result_free(uapmd_smf_convert_result_t* result);

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiClipReader
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_smf_convert_result_t* uapmd_midi_clip_read_any_format(const char* file_path);
UAPMD_C_EXPORT bool uapmd_midi_clip_is_valid_smf2(const char* file_path);
UAPMD_C_EXPORT bool uapmd_midi_clip_is_valid_smf(const char* file_path);

/* ═══════════════════════════════════════════════════════════════════════════
 *  ProjectArchive
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT bool uapmd_project_archive_is_archive(const char* path);

typedef struct uapmd_project_archive_extract_result {
    bool success;
    const char* error;
    const char* project_file;
} uapmd_project_archive_extract_result_t;

UAPMD_C_EXPORT uapmd_project_archive_extract_result_t* uapmd_project_archive_extract(const char* archive_path, const char* destination_dir);
UAPMD_C_EXPORT void uapmd_project_archive_extract_result_free(uapmd_project_archive_extract_result_t* result);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdProjectData (project file read/write)
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_project_data_t uapmd_project_data_create(void);
UAPMD_C_EXPORT void uapmd_project_data_destroy(uapmd_project_data_t data);

UAPMD_C_EXPORT uint32_t uapmd_project_data_track_count(uapmd_project_data_t data);
UAPMD_C_EXPORT uapmd_project_track_data_t uapmd_project_data_get_track(uapmd_project_data_t data, uint32_t index);
UAPMD_C_EXPORT uapmd_project_track_data_t uapmd_project_data_master_track(uapmd_project_data_t data);
UAPMD_C_EXPORT bool uapmd_project_data_remove_track(uapmd_project_data_t data, uint32_t index);

UAPMD_C_EXPORT bool uapmd_project_data_write(uapmd_project_data_t data, const char* file_path);
UAPMD_C_EXPORT uapmd_project_data_t uapmd_project_data_read(const char* file_path);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdProjectPluginNodeData (for project graph)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_project_plugin_node_data {
    const char* plugin_id;
    const char* format;
    const char* display_name;
    const char* state_file;
    int32_t group_index;
} uapmd_project_plugin_node_data_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdProjectPluginGraphData
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_project_graph_data_t uapmd_project_graph_data_create(void);
UAPMD_C_EXPORT size_t uapmd_project_graph_get_graph_type(uapmd_project_graph_data_t graph, char* buf, size_t buf_size);
UAPMD_C_EXPORT void   uapmd_project_graph_set_graph_type(uapmd_project_graph_data_t graph, const char* type);
UAPMD_C_EXPORT size_t uapmd_project_graph_get_external_file(uapmd_project_graph_data_t graph, char* buf, size_t buf_size);
UAPMD_C_EXPORT void   uapmd_project_graph_set_external_file(uapmd_project_graph_data_t graph, const char* path);

/* ═══════════════════════════════════════════════════════════════════════════
 *  TimelineTrack
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_clip_manager_t uapmd_tt_clip_manager(uapmd_timeline_track_t tt);
UAPMD_C_EXPORT uint32_t uapmd_tt_channel_count(uapmd_timeline_track_t tt);
UAPMD_C_EXPORT double   uapmd_tt_sample_rate(uapmd_timeline_track_t tt);
UAPMD_C_EXPORT size_t   uapmd_tt_reference_id(uapmd_timeline_track_t tt, char* buf, size_t buf_size);
UAPMD_C_EXPORT bool     uapmd_tt_has_device_input_source(uapmd_timeline_track_t tt);
UAPMD_C_EXPORT bool     uapmd_tt_remove_clip(uapmd_timeline_track_t tt, int32_t clip_id);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_DATA_H */
