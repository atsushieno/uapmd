#include "uapmd_wasm_api.h"
#include <cstring>
#include <mutex>
#include <unordered_map>

static std::mutex g_mutex;
static std::unordered_map<void*, void*> g_instances;
static int g_instance_counter = 0;

extern "C" {

void* uapmd_create(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    void* handle = reinterpret_cast<void*>(++g_instance_counter);
    g_instances[handle] = handle;
    return handle;
}

void uapmd_destroy(void* instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances.erase(instance);
}

int uapmd_scan_plugins(void* instance) {
    return g_instances.count(instance) ? 0 : -1;
}

int uapmd_get_plugin_count(void* instance) {
    return g_instances.count(instance) ? 0 : 0;
}

const char* uapmd_get_plugin_name(void* instance, int index) {
    static thread_local char buffer[256];
    if (!g_instances.count(instance) || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    std::strncpy(buffer, "Demo Plugin", sizeof(buffer) - 1);
    return buffer;
}

const char* uapmd_get_plugin_vendor(void* instance, int index) {
    static thread_local char buffer[256];
    if (!g_instances.count(instance) || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    std::strncpy(buffer, "Demo Vendor", sizeof(buffer) - 1);
    return buffer;
}

const char* uapmd_get_plugin_format(void* instance, int index) {
    static thread_local char buffer[32];
    if (!g_instances.count(instance) || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    std::strncpy(buffer, "VST3", sizeof(buffer) - 1);
    return buffer;
}

const char* uapmd_get_plugin_path(void* instance, int index) {
    static thread_local char buffer[512];
    if (!g_instances.count(instance) || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    std::strncpy(buffer, "/path/to/plugin.vst3", sizeof(buffer) - 1);
    return buffer;
}

int uapmd_load_plugin(void* instance, int index) {
    return g_instances.count(instance) ? 0 : -1;
}

int uapmd_start_audio(void* instance, int sample_rate, int buffer_size) {
    return g_instances.count(instance) ? 0 : -1;
}

int uapmd_stop_audio(void* instance) {
    return g_instances.count(instance) ? 0 : -1;
}

int uapmd_process_audio(void* instance, float* input, float* output, int frames) {
    if (!g_instances.count(instance)) return -1;
    if (input && output) {
        for (int i = 0; i < frames; ++i) {
            output[i * 2] = input[i * 2];
            output[i * 2 + 1] = input[i * 2 + 1];
        }
    }
    return 0;
}

int uapmd_get_parameter_count(void* instance) {
    return g_instances.count(instance) ? 0 : 0;
}

const char* uapmd_get_parameter_name(void* instance, int param_index) {
    static thread_local char buffer[256];
    if (!g_instances.count(instance) || param_index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    std::strncpy(buffer, "Parameter", sizeof(buffer) - 1);
    return buffer;
}

float uapmd_get_parameter_value(void* instance, int param_index) {
    return g_instances.count(instance) ? 0.0f : 0.0f;
}

int uapmd_set_parameter_value(void* instance, int param_index, float value) {
    return g_instances.count(instance) ? 0 : -1;
}

int uapmd_send_midi(void* instance, const unsigned char* data, int length) {
    return (g_instances.count(instance) && data && length > 0) ? 0 : -1;
}

}
