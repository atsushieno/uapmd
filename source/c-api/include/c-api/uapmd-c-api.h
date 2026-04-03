/* uapmd C API — bindings for the uapmd module */
#ifndef UAPMD_C_API_H
#define UAPMD_C_API_H

#include "uapmd-c-common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginInstanceAPI
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT size_t uapmd_instance_display_name(uapmd_plugin_instance_t inst, char* buf, size_t buf_size);
UAPMD_C_EXPORT size_t uapmd_instance_format_name(uapmd_plugin_instance_t inst, char* buf, size_t buf_size);
UAPMD_C_EXPORT size_t uapmd_instance_plugin_id(uapmd_plugin_instance_t inst, char* buf, size_t buf_size);

UAPMD_C_EXPORT bool uapmd_instance_get_bypassed(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT void uapmd_instance_set_bypassed(uapmd_plugin_instance_t inst, bool value);

UAPMD_C_EXPORT uapmd_status_t uapmd_instance_start_processing(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT uapmd_status_t uapmd_instance_stop_processing(uapmd_plugin_instance_t inst);

UAPMD_C_EXPORT uint32_t uapmd_instance_latency_in_samples(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT double   uapmd_instance_tail_length_in_seconds(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT bool     uapmd_instance_requires_replacing_process(uapmd_plugin_instance_t inst);

/* Parameters */
UAPMD_C_EXPORT uint32_t uapmd_instance_parameter_count(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT bool     uapmd_instance_get_parameter_metadata(uapmd_plugin_instance_t inst, uint32_t list_index, uapmd_parameter_metadata_t* out);
UAPMD_C_EXPORT double   uapmd_instance_get_parameter_value(uapmd_plugin_instance_t inst, int32_t index);
UAPMD_C_EXPORT void     uapmd_instance_set_parameter_value(uapmd_plugin_instance_t inst, int32_t index, double value);
UAPMD_C_EXPORT size_t   uapmd_instance_get_parameter_value_string(uapmd_plugin_instance_t inst, int32_t index, double value, char* buf, size_t buf_size);

/* Per-note controllers */
UAPMD_C_EXPORT void   uapmd_instance_set_per_note_controller_value(uapmd_plugin_instance_t inst, uint8_t note, uint8_t index, double value);
UAPMD_C_EXPORT size_t uapmd_instance_get_per_note_controller_value_string(uapmd_plugin_instance_t inst, uint8_t note, uint8_t index, double value, char* buf, size_t buf_size);

/* Presets */
UAPMD_C_EXPORT uint32_t uapmd_instance_preset_count(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT bool     uapmd_instance_get_preset_metadata(uapmd_plugin_instance_t inst, uint32_t list_index, uapmd_preset_metadata_t* out);
UAPMD_C_EXPORT void     uapmd_instance_load_preset(uapmd_plugin_instance_t inst, int32_t preset_index);

/* State (synchronous, legacy) */
UAPMD_C_EXPORT size_t uapmd_instance_save_state_sync(uapmd_plugin_instance_t inst, uint8_t* buf, size_t buf_size);
UAPMD_C_EXPORT void   uapmd_instance_load_state_sync(uapmd_plugin_instance_t inst, const uint8_t* data, size_t data_size);

/* State (asynchronous) */
typedef void (*uapmd_request_state_cb_t)(const uint8_t* state, size_t state_size, const char* error, void* user_data);
typedef void (*uapmd_load_state_cb_t)(const char* error, void* user_data);

UAPMD_C_EXPORT void uapmd_instance_request_state(uapmd_plugin_instance_t inst,
                                                   uapmd_state_context_type_t ctx,
                                                   bool include_ui_state,
                                                   void* user_data,
                                                   uapmd_request_state_cb_t callback);

UAPMD_C_EXPORT void uapmd_instance_load_state(uapmd_plugin_instance_t inst,
                                                const uint8_t* state, size_t state_size,
                                                uapmd_state_context_type_t ctx,
                                                bool include_ui_state,
                                                void* user_data,
                                                uapmd_load_state_cb_t callback);

/* UI */
UAPMD_C_EXPORT bool uapmd_instance_has_ui_support(uapmd_plugin_instance_t inst);

typedef bool (*uapmd_ui_resize_handler_t)(uint32_t width, uint32_t height, void* user_data);

UAPMD_C_EXPORT bool uapmd_instance_create_ui(uapmd_plugin_instance_t inst,
                                               bool is_floating,
                                               void* parent_handle,
                                               void* resize_user_data,
                                               uapmd_ui_resize_handler_t resize_handler);
UAPMD_C_EXPORT void uapmd_instance_destroy_ui(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT bool uapmd_instance_show_ui(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT void uapmd_instance_hide_ui(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT bool uapmd_instance_is_ui_visible(uapmd_plugin_instance_t inst);
UAPMD_C_EXPORT bool uapmd_instance_set_ui_size(uapmd_plugin_instance_t inst, uint32_t width, uint32_t height);
UAPMD_C_EXPORT bool uapmd_instance_get_ui_size(uapmd_plugin_instance_t inst, uint32_t* width, uint32_t* height);
UAPMD_C_EXPORT bool uapmd_instance_can_ui_resize(uapmd_plugin_instance_t inst);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginHostingAPI
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_plugin_host_t uapmd_plugin_host_create(void);
UAPMD_C_EXPORT void uapmd_plugin_host_destroy(uapmd_plugin_host_t host);

UAPMD_C_EXPORT uint32_t uapmd_plugin_host_catalog_entry_count(uapmd_plugin_host_t host);
/* Returns plugin catalog info at `index`. Writes format/pluginId/displayName into provided buffers. */
UAPMD_C_EXPORT bool uapmd_plugin_host_get_catalog_entry(uapmd_plugin_host_t host, uint32_t index,
                                                          char* format_buf, size_t format_buf_size,
                                                          char* plugin_id_buf, size_t plugin_id_buf_size,
                                                          char* display_name_buf, size_t display_name_buf_size);

UAPMD_C_EXPORT void uapmd_plugin_host_save_catalog(uapmd_plugin_host_t host, const char* path);
UAPMD_C_EXPORT void uapmd_plugin_host_perform_scanning(uapmd_plugin_host_t host, bool rescan);
UAPMD_C_EXPORT void uapmd_plugin_host_reload_catalog_from_cache(uapmd_plugin_host_t host);

typedef void (*uapmd_create_instance_cb_t)(int32_t instance_id, const char* error, void* user_data);

UAPMD_C_EXPORT void uapmd_plugin_host_create_instance(uapmd_plugin_host_t host,
                                                        uint32_t sample_rate,
                                                        uint32_t buffer_size,
                                                        int32_t main_input_channels,   /* -1 for none */
                                                        int32_t main_output_channels,  /* -1 for none */
                                                        bool offline_mode,
                                                        const char* format,
                                                        const char* plugin_id,
                                                        void* user_data,
                                                        uapmd_create_instance_cb_t callback);

UAPMD_C_EXPORT void uapmd_plugin_host_delete_instance(uapmd_plugin_host_t host, int32_t instance_id);
UAPMD_C_EXPORT uapmd_plugin_instance_t uapmd_plugin_host_get_instance(uapmd_plugin_host_t host, int32_t instance_id);

UAPMD_C_EXPORT uint32_t uapmd_plugin_host_instance_id_count(uapmd_plugin_host_t host);
UAPMD_C_EXPORT bool     uapmd_plugin_host_get_instance_ids(uapmd_plugin_host_t host, int32_t* out, uint32_t out_count);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginNode
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT int32_t                uapmd_node_instance_id(uapmd_plugin_node_t node);
UAPMD_C_EXPORT uapmd_plugin_instance_t uapmd_node_instance(uapmd_plugin_node_t node);
UAPMD_C_EXPORT bool  uapmd_node_schedule_events(uapmd_plugin_node_t node, uapmd_timestamp_t timestamp, void* events, size_t size);
UAPMD_C_EXPORT void  uapmd_node_send_all_notes_off(uapmd_plugin_node_t node);

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioPluginGraph
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_plugin_graph_t uapmd_graph_create(size_t event_buffer_size_in_bytes);
UAPMD_C_EXPORT void uapmd_graph_destroy(uapmd_plugin_graph_t graph);

typedef void (*uapmd_graph_delete_cb_t)(void* user_data);

UAPMD_C_EXPORT uapmd_status_t uapmd_graph_append_node(uapmd_plugin_graph_t graph,
                                                        int32_t instance_id,
                                                        uapmd_plugin_instance_t instance,
                                                        void* delete_user_data,
                                                        uapmd_graph_delete_cb_t on_delete);
UAPMD_C_EXPORT bool uapmd_graph_remove_node(uapmd_plugin_graph_t graph, int32_t instance_id);

UAPMD_C_EXPORT uint32_t           uapmd_graph_plugin_count(uapmd_plugin_graph_t graph);
UAPMD_C_EXPORT uapmd_plugin_node_t uapmd_graph_get_plugin_node(uapmd_plugin_graph_t graph, int32_t instance_id);

typedef void (*uapmd_event_output_cb_t)(int32_t instance_id, const uapmd_ump_t* data, size_t data_size_in_bytes, void* user_data);

UAPMD_C_EXPORT void uapmd_graph_set_event_output_callback(uapmd_plugin_graph_t graph,
                                                            void* user_data,
                                                            uapmd_event_output_cb_t callback);

UAPMD_C_EXPORT uint32_t uapmd_graph_output_bus_count(uapmd_plugin_graph_t graph);
UAPMD_C_EXPORT uint32_t uapmd_graph_output_latency_in_samples(uapmd_plugin_graph_t graph, uint32_t bus_index);
UAPMD_C_EXPORT double   uapmd_graph_output_tail_length_in_seconds(uapmd_plugin_graph_t graph, uint32_t bus_index);
UAPMD_C_EXPORT uint32_t uapmd_graph_render_lead_in_samples(uapmd_plugin_graph_t graph);
UAPMD_C_EXPORT uint32_t uapmd_graph_main_output_latency_in_samples(uapmd_plugin_graph_t graph);
UAPMD_C_EXPORT double   uapmd_graph_main_output_tail_length_in_seconds(uapmd_plugin_graph_t graph);

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiIOFeature
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void uapmd_midi_io_add_input_handler(uapmd_midi_io_t io, uapmd_ump_receiver_t receiver, void* user_data);
UAPMD_C_EXPORT void uapmd_midi_io_remove_input_handler(uapmd_midi_io_t io, uapmd_ump_receiver_t receiver);
UAPMD_C_EXPORT void uapmd_midi_io_send(uapmd_midi_io_t io, uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdFunctionBlock
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_midi_io_t uapmd_fb_midi_io(uapmd_function_block_t fb);
UAPMD_C_EXPORT int32_t  uapmd_fb_instance_id(uapmd_function_block_t fb);
UAPMD_C_EXPORT uint8_t  uapmd_fb_get_group(uapmd_function_block_t fb);
UAPMD_C_EXPORT void     uapmd_fb_set_group(uapmd_function_block_t fb, uint8_t group_id);
UAPMD_C_EXPORT void     uapmd_fb_detach_output_mapper(uapmd_function_block_t fb);
UAPMD_C_EXPORT void     uapmd_fb_initialize(uapmd_function_block_t fb);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdFunctionBlockManager
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT size_t  uapmd_fbm_count(uapmd_function_block_mgr_t mgr);
UAPMD_C_EXPORT size_t  uapmd_fbm_create_device(uapmd_function_block_mgr_t mgr);
UAPMD_C_EXPORT uapmd_function_device_t uapmd_fbm_get_device_by_index(uapmd_function_block_mgr_t mgr, int32_t index);
UAPMD_C_EXPORT uapmd_function_device_t uapmd_fbm_get_device_for_instance(uapmd_function_block_mgr_t mgr, int32_t instance_id);
UAPMD_C_EXPORT void    uapmd_fbm_delete_empty_devices(uapmd_function_block_mgr_t mgr);
UAPMD_C_EXPORT void    uapmd_fbm_detach_all_output_mappers(uapmd_function_block_mgr_t mgr);
UAPMD_C_EXPORT void    uapmd_fbm_clear_all_devices(uapmd_function_block_mgr_t mgr);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdUmpInputMapper / UapmdUmpOutputMapper
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT void   uapmd_ump_in_set_parameter_value(uapmd_ump_input_mapper_t m, uint16_t index, double value);
UAPMD_C_EXPORT double uapmd_ump_in_get_parameter_value(uapmd_ump_input_mapper_t m, uint16_t index);
UAPMD_C_EXPORT void   uapmd_ump_in_set_per_note_controller_value(uapmd_ump_input_mapper_t m, uint8_t note, uint8_t index, double value);
UAPMD_C_EXPORT void   uapmd_ump_in_load_preset(uapmd_ump_input_mapper_t m, uint32_t index);

UAPMD_C_EXPORT void uapmd_ump_out_send_parameter_value(uapmd_ump_output_mapper_t m, uint16_t index, double value);
UAPMD_C_EXPORT void uapmd_ump_out_send_per_note_controller_value(uapmd_ump_output_mapper_t m, uint8_t note, uint8_t index, double value);
UAPMD_C_EXPORT void uapmd_ump_out_send_preset_index_change(uapmd_ump_output_mapper_t m, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_API_H */
