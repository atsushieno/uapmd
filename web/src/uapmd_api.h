#ifndef UAPMD_API_H
#define UAPMD_API_H

#ifdef __cplusplus
extern "C" {
#endif

// Instance management
void* uapmd_create(void);
void uapmd_destroy(void* instance);

// Plugin scanning
int uapmd_scan_plugins(void* instance);
int uapmd_get_plugin_count(void* instance);

// Plugin information
const char* uapmd_get_plugin_name(void* instance, int index);
const char* uapmd_get_plugin_vendor(void* instance, int index);
const char* uapmd_get_plugin_format(void* instance, int index);
const char* uapmd_get_plugin_path(void* instance, int index);

// Plugin loading
int uapmd_load_plugin(void* instance, int index);

// Audio processing
int uapmd_start_audio(void* instance, int sample_rate, int buffer_size);
int uapmd_stop_audio(void* instance);
int uapmd_process_audio(void* instance, float* input, float* output, int frames);

// Parameter control
int uapmd_get_parameter_count(void* instance);
const char* uapmd_get_parameter_name(void* instance, int param_index);
float uapmd_get_parameter_value(void* instance, int param_index);
int uapmd_set_parameter_value(void* instance, int param_index, float value);

// MIDI handling
int uapmd_send_midi(void* instance, const unsigned char* data, int length);

#ifdef __cplusplus
}
#endif

#endif // UAPMD_API_H
