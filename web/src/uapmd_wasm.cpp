#include "uapmd_api.h"
#include <cstring>
#include <memory>
#include <unordered_map>
#include <mutex>

// Stub implementation for initial Wasm build
// Full integration will link with actual remidy library

// Global state
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
    auto it = g_instances.find(instance);
    if (it != g_instances.end()) {
        g_instances.erase(it);
    }
}

int uapmd_scan_plugins(void* instance) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return -1;
    return 0;
}

int uapmd_get_plugin_count(void* instance) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return 0;
    return 0;
}

const char* uapmd_get_plugin_name(void* instance, int index) {
    static thread_local char buffer[256];
    auto it = g_instances.find(instance);
    
    if (it == g_instances.end() || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    
    strncpy(buffer, "Demo Plugin", sizeof(buffer) - 1);
    return buffer;
}

const char* uapmd_get_plugin_vendor(void* instance, int index) {
    static thread_local char buffer[256];
    auto it = g_instances.find(instance);
    
    if (it == g_instances.end() || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    
    strncpy(buffer, "Demo Vendor", sizeof(buffer) - 1);
    return buffer;
}

const char* uapmd_get_plugin_format(void* instance, int index) {
    static thread_local char buffer[32];
    auto it = g_instances.find(instance);
    
    if (it == g_instances.end() || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    
    strncpy(buffer, "VST3", sizeof(buffer) - 1);
    return buffer;
}

const char* uapmd_get_plugin_path(void* instance, int index) {
    static thread_local char buffer[512];
    auto it = g_instances.find(instance);
    
    if (it == g_instances.end() || index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    
    strncpy(buffer, "/path/to/plugin.vst3", sizeof(buffer) - 1);
    return buffer;
}

int uapmd_load_plugin(void* instance, int index) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return -1;
    return 0;
}

int uapmd_start_audio(void* instance, int sample_rate, int buffer_size) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return -1;
    return 0;
}

int uapmd_stop_audio(void* instance) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return -1;
    return 0;
}

int uapmd_process_audio(void* instance, float* input, float* output, int frames) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return -1;
    
    if (input && output) {
        for (int i = 0; i < frames; ++i) {
            output[i * 2] = input[i * 2];
            output[i * 2 + 1] = input[i * 2 + 1];
        }
    }
    return 0;
}

int uapmd_get_parameter_count(void* instance) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return 0;
    return 0;
}

const char* uapmd_get_parameter_name(void* instance, int param_index) {
    static thread_local char buffer[256];
    auto it = g_instances.find(instance);
    
    if (it == g_instances.end() || param_index < 0) {
        buffer[0] = '\0';
        return buffer;
    }
    
    strncpy(buffer, "Parameter", sizeof(buffer) - 1);
    return buffer;
}

float uapmd_get_parameter_value(void* instance, int param_index) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return 0.0f;
    return 0.0f;
}

int uapmd_set_parameter_value(void* instance, int param_index, float value) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return -1;
    return 0;
}

int uapmd_send_midi(void* instance, const unsigned char* data, int length) {
    auto it = g_instances.find(instance);
    if (it == g_instances.end() || !data || length < 1) return -1;
    return 0;
}

}
