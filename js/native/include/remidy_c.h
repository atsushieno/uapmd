#ifndef REMIDY_C_H
#define REMIDY_C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct RemidyPluginCatalog RemidyPluginCatalog;
typedef struct RemidyPluginCatalogEntry RemidyPluginCatalogEntry;
typedef struct RemidyPluginFormat RemidyPluginFormat;
typedef struct RemidyPluginInstance RemidyPluginInstance;
typedef struct RemidyPluginScanTool RemidyPluginScanTool;
typedef struct RemidyContainerWindow RemidyContainerWindow;
typedef struct RemidyGLContextGuard RemidyGLContextGuard;

// Status codes
typedef enum {
    REMIDY_OK = 0,
    REMIDY_ERROR = 1,
    REMIDY_NOT_SUPPORTED = 2,
    REMIDY_INVALID_PARAMETER = 3,
} RemidyStatusCode;

// ========== PluginCatalog API ==========

RemidyPluginCatalog* remidy_catalog_create(void);
void remidy_catalog_destroy(RemidyPluginCatalog* catalog);
void remidy_catalog_clear(RemidyPluginCatalog* catalog);
RemidyStatusCode remidy_catalog_load(RemidyPluginCatalog* catalog, const char* path);
RemidyStatusCode remidy_catalog_save(RemidyPluginCatalog* catalog, const char* path);
int remidy_catalog_get_plugin_count(RemidyPluginCatalog* catalog);
RemidyPluginCatalogEntry* remidy_catalog_get_plugin_at(RemidyPluginCatalog* catalog, int index);

// ========== PluginCatalogEntry API ==========

const char* remidy_entry_get_format(RemidyPluginCatalogEntry* entry);
const char* remidy_entry_get_plugin_id(RemidyPluginCatalogEntry* entry);
const char* remidy_entry_get_display_name(RemidyPluginCatalogEntry* entry);
const char* remidy_entry_get_vendor_name(RemidyPluginCatalogEntry* entry);
const char* remidy_entry_get_product_url(RemidyPluginCatalogEntry* entry);
const char* remidy_entry_get_bundle_path(RemidyPluginCatalogEntry* entry);

// ========== PluginScanTool API ==========

RemidyPluginScanTool* remidy_scan_tool_create(void);
void remidy_scan_tool_destroy(RemidyPluginScanTool* tool);
RemidyPluginCatalog* remidy_scan_tool_get_catalog(RemidyPluginScanTool* tool);
RemidyStatusCode remidy_scan_tool_perform_scanning(RemidyPluginScanTool* tool);
RemidyStatusCode remidy_scan_tool_save_cache(RemidyPluginScanTool* tool, const char* path);
void remidy_scan_tool_set_cache_file(RemidyPluginScanTool* tool, const char* path);

// ========== PluginFormat API ==========

typedef struct {
    const char* name;
    RemidyPluginFormat* handle;
} RemidyPluginFormatInfo;

int remidy_scan_tool_get_format_count(RemidyPluginScanTool* tool);
RemidyPluginFormatInfo remidy_scan_tool_get_format_at(RemidyPluginScanTool* tool, int index);

// ========== PluginInstance API ==========

typedef struct {
    uint32_t sample_rate;
    uint32_t buffer_size_in_samples;
    bool offline_mode;
    uint32_t main_input_channels;
    uint32_t main_output_channels;
    bool has_main_input_channels;
    bool has_main_output_channels;
} RemidyConfigurationRequest;

// Callback type for async instance creation
typedef void (*RemidyInstanceCreateCallback)(
    RemidyPluginInstance* instance,
    const char* error,
    void* user_data
);

// Synchronous instance creation (may deadlock if plugin requires main thread)
RemidyPluginInstance* remidy_instance_create(
    RemidyPluginFormat* format,
    RemidyPluginCatalogEntry* entry
);

// Asynchronous instance creation with callback
void remidy_instance_create_async(
    RemidyPluginFormat* format,
    RemidyPluginCatalogEntry* entry,
    RemidyInstanceCreateCallback callback,
    void* user_data
);

void remidy_instance_destroy(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_configure(
    RemidyPluginInstance* instance,
    RemidyConfigurationRequest* config
);
RemidyStatusCode remidy_instance_start_processing(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_stop_processing(RemidyPluginInstance* instance);

// ========== Parameter API ==========

typedef struct {
    uint32_t id;
    char name[256];
    double min_value;
    double max_value;
    double default_value;
    bool is_automatable;
    bool is_readonly;
} RemidyParameterInfo;

int remidy_instance_get_parameter_count(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_get_parameter_info(
    RemidyPluginInstance* instance,
    int index,
    RemidyParameterInfo* info
);
RemidyStatusCode remidy_instance_get_parameter_value(
    RemidyPluginInstance* instance,
    uint32_t param_id,
    double* value
);
RemidyStatusCode remidy_instance_set_parameter_value(
    RemidyPluginInstance* instance,
    uint32_t param_id,
    double value
);

// ========== Plugin UI API ==========

typedef bool (*RemidyUIResizeCallback)(uint32_t width, uint32_t height, void* user_data);

bool remidy_instance_has_ui(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_create_ui(
    RemidyPluginInstance* instance,
    bool is_floating,
    uintptr_t parent_handle,
    RemidyUIResizeCallback resize_callback,
    void* user_data
);
void remidy_instance_destroy_ui(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_show_ui(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_hide_ui(RemidyPluginInstance* instance);
RemidyStatusCode remidy_instance_get_ui_size(
    RemidyPluginInstance* instance,
    uint32_t* width,
    uint32_t* height
);
RemidyStatusCode remidy_instance_set_ui_size(
    RemidyPluginInstance* instance,
    uint32_t width,
    uint32_t height
);
bool remidy_instance_can_ui_resize(RemidyPluginInstance* instance);

// ========== Container Window API ==========

typedef struct {
    int x;
    int y;
    int width;
    int height;
} RemidyBounds;

typedef void (*RemidyContainerWindowCloseCallback)(void* user_data);

RemidyContainerWindow* remidy_container_window_create(
    const char* title,
    int width,
    int height,
    RemidyContainerWindowCloseCallback callback,
    void* user_data
);
void remidy_container_window_destroy(RemidyContainerWindow* window);
void remidy_container_window_show(RemidyContainerWindow* window, bool visible);
void remidy_container_window_resize(RemidyContainerWindow* window, int width, int height);
RemidyBounds remidy_container_window_get_bounds(RemidyContainerWindow* window);
uintptr_t remidy_container_window_get_handle(RemidyContainerWindow* window);
RemidyGLContextGuard* remidy_gl_context_guard_create(void);
void remidy_gl_context_guard_destroy(RemidyGLContextGuard* guard);

// ========== EventLoop API ==========

// Callback type for enqueueing tasks on main thread
typedef void (*RemidyMainThreadTask)(void* user_data);

// Initialize the event loop for Node.js environment
// This should be called once at startup from the Node.js main thread
void remidy_eventloop_init_nodejs(
    void (*enqueue_callback)(RemidyMainThreadTask task, void* user_data, void* context),
    void* context
);

#ifdef __cplusplus
}
#endif

#endif // REMIDY_C_H
