import * as koffi from 'koffi';
import * as path from 'path';
import * as os from 'os';

// Determine library name based on platform
function getLibraryPath(): string {
    const platform = os.platform();
    const ext = platform === 'darwin' ? 'dylib' : platform === 'win32' ? 'dll' : 'so';

    // Find the package root by looking for where src/ directory is
    // When running from dist/src/ffi.js, __dirname is dist/src
    // When running from dist/examples/*.js, __dirname is dist/examples
    let packageRoot = __dirname;

    // Navigate up to find the package root (where package.json is)
    while (packageRoot !== path.dirname(packageRoot)) {
        const srcPath = path.join(packageRoot, 'src');
        const pkgPath = path.join(packageRoot, 'package.json');
        if (require('fs').existsSync(pkgPath)) {
            break;
        }
        packageRoot = path.dirname(packageRoot);
    }

    // Try different possible locations
    const possiblePaths = [
        path.join(packageRoot, 'dist', 'native', `libremidy_c.${ext}`),
        path.join(packageRoot, 'native', 'build', `libremidy_c.${ext}`),
        `libremidy_c.${ext}`, // System path
    ];

    for (const libPath of possiblePaths) {
        try {
            return libPath;
        } catch (e) {
            continue;
        }
    }

    throw new Error('Could not find remidy_c library. Please build the native library first.');
}

// Load the native library
const lib = koffi.load(getLibraryPath());

// Status codes enum
export enum StatusCode {
    OK = 0,
    ERROR = 1,
    NOT_SUPPORTED = 2,
    INVALID_PARAMETER = 3,
}

// Opaque pointer types
export type PluginCatalogHandle = koffi.IKoffiCType;
export type PluginCatalogEntryHandle = koffi.IKoffiCType;
export type PluginFormatHandle = koffi.IKoffiCType;
export type PluginInstanceHandle = koffi.IKoffiCType;
export type PluginScanToolHandle = koffi.IKoffiCType;
export type ContainerWindowHandle = koffi.IKoffiCType;
export type GLContextGuardHandle = koffi.IKoffiCType;

// Define opaque pointer types
const RemidyPluginCatalog = koffi.opaque('RemidyPluginCatalog');
const RemidyPluginCatalogEntry = koffi.opaque('RemidyPluginCatalogEntry');
const RemidyPluginFormat = koffi.opaque('RemidyPluginFormat');
const RemidyPluginInstance = koffi.opaque('RemidyPluginInstance');
const RemidyPluginScanTool = koffi.opaque('RemidyPluginScanTool');
const RemidyContainerWindow = koffi.opaque('RemidyContainerWindow');
const RemidyGLContextGuard = koffi.opaque('RemidyGLContextGuard');

// Struct definitions
const RemidyConfigurationRequest = koffi.struct('RemidyConfigurationRequest', {
    sample_rate: 'uint32_t',
    buffer_size_in_samples: 'uint32_t',
    offline_mode: 'bool',
    main_input_channels: 'uint32_t',
    main_output_channels: 'uint32_t',
    has_main_input_channels: 'bool',
    has_main_output_channels: 'bool',
});

const RemidyParameterInfo = koffi.struct('RemidyParameterInfo', {
    id: 'uint32_t',
    name: koffi.array('char', 256),
    min_value: 'double',
    max_value: 'double',
    default_value: 'double',
    is_automatable: 'bool',
    is_readonly: 'bool',
});

const RemidyPluginFormatInfo = koffi.struct('RemidyPluginFormatInfo', {
    name: 'string',
    handle: koffi.pointer(RemidyPluginFormat),
});

const RemidyBounds = koffi.struct('RemidyBounds', {
    x: 'int',
    y: 'int',
    width: 'int',
    height: 'int',
});

// ========== PluginCatalog API ==========

export const remidy_catalog_create = lib.func('remidy_catalog_create',
    koffi.pointer(RemidyPluginCatalog), []);
export const remidy_catalog_destroy = lib.func('remidy_catalog_destroy',
    'void', [koffi.pointer(RemidyPluginCatalog)]);
export const remidy_catalog_clear = lib.func('remidy_catalog_clear',
    'void', [koffi.pointer(RemidyPluginCatalog)]);
export const remidy_catalog_load = lib.func('remidy_catalog_load',
    'int', [koffi.pointer(RemidyPluginCatalog), 'string']);
export const remidy_catalog_save = lib.func('remidy_catalog_save',
    'int', [koffi.pointer(RemidyPluginCatalog), 'string']);
export const remidy_catalog_get_plugin_count = lib.func('remidy_catalog_get_plugin_count',
    'int', [koffi.pointer(RemidyPluginCatalog)]);
export const remidy_catalog_get_plugin_at = lib.func('remidy_catalog_get_plugin_at',
    koffi.pointer(RemidyPluginCatalogEntry), [koffi.pointer(RemidyPluginCatalog), 'int']);

// ========== PluginCatalogEntry API ==========

export const remidy_entry_get_format = lib.func('remidy_entry_get_format',
    'string', [koffi.pointer(RemidyPluginCatalogEntry)]);
export const remidy_entry_get_plugin_id = lib.func('remidy_entry_get_plugin_id',
    'string', [koffi.pointer(RemidyPluginCatalogEntry)]);
export const remidy_entry_get_display_name = lib.func('remidy_entry_get_display_name',
    'string', [koffi.pointer(RemidyPluginCatalogEntry)]);
export const remidy_entry_get_vendor_name = lib.func('remidy_entry_get_vendor_name',
    'string', [koffi.pointer(RemidyPluginCatalogEntry)]);
export const remidy_entry_get_product_url = lib.func('remidy_entry_get_product_url',
    'string', [koffi.pointer(RemidyPluginCatalogEntry)]);
export const remidy_entry_get_bundle_path = lib.func('remidy_entry_get_bundle_path',
    'string', [koffi.pointer(RemidyPluginCatalogEntry)]);

// ========== PluginScanTool API ==========

export const remidy_scan_tool_create = lib.func('remidy_scan_tool_create',
    koffi.pointer(RemidyPluginScanTool), []);
export const remidy_scan_tool_destroy = lib.func('remidy_scan_tool_destroy',
    'void', [koffi.pointer(RemidyPluginScanTool)]);
export const remidy_scan_tool_get_catalog = lib.func('remidy_scan_tool_get_catalog',
    koffi.pointer(RemidyPluginCatalog), [koffi.pointer(RemidyPluginScanTool)]);
export const remidy_scan_tool_perform_scanning = lib.func('remidy_scan_tool_perform_scanning',
    'int', [koffi.pointer(RemidyPluginScanTool)]);
export const remidy_scan_tool_save_cache = lib.func('remidy_scan_tool_save_cache',
    'int', [koffi.pointer(RemidyPluginScanTool), 'string']);
export const remidy_scan_tool_set_cache_file = lib.func('remidy_scan_tool_set_cache_file',
    'void', [koffi.pointer(RemidyPluginScanTool), 'string']);

// ========== PluginFormat API ==========

export const remidy_scan_tool_get_format_count = lib.func('remidy_scan_tool_get_format_count',
    'int', [koffi.pointer(RemidyPluginScanTool)]);
export const remidy_scan_tool_get_format_at = lib.func('remidy_scan_tool_get_format_at',
    RemidyPluginFormatInfo, [koffi.pointer(RemidyPluginScanTool), 'int']);

// ========== PluginInstance API ==========

// Callback type for async instance creation
export type InstanceCreateCallback = (
    instance: PluginInstanceHandle | null,
    error: string | null,
    userData: any
) => void;

// Define the callback signature for koffi
const RemidyInstanceCreateCallback = koffi.proto('void RemidyInstanceCreateCallback(void* instance, const char* error, void* user_data)');
const RemidyUIResizeCallback = koffi.proto('bool RemidyUIResizeCallback(uint32_t width, uint32_t height, void* user_data)');
const RemidyContainerWindowCloseCallback = koffi.proto('void RemidyContainerWindowCloseCallback(void* user_data)');

export const remidy_instance_create = lib.func('remidy_instance_create',
    koffi.pointer(RemidyPluginInstance),
    [koffi.pointer(RemidyPluginFormat), koffi.pointer(RemidyPluginCatalogEntry)]);

export const remidy_instance_create_async = lib.func('remidy_instance_create_async',
    'void',
    [koffi.pointer(RemidyPluginFormat), koffi.pointer(RemidyPluginCatalogEntry),
     koffi.pointer(RemidyInstanceCreateCallback), koffi.pointer('void')]);

export const remidy_instance_destroy = lib.func('remidy_instance_destroy',
    'void', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_configure = lib.func('remidy_instance_configure',
    'int', [koffi.pointer(RemidyPluginInstance), koffi.pointer(RemidyConfigurationRequest)]);
export const remidy_instance_start_processing = lib.func('remidy_instance_start_processing',
    'int', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_stop_processing = lib.func('remidy_instance_stop_processing',
    'int', [koffi.pointer(RemidyPluginInstance)]);

// ========== Parameter API ==========

export const remidy_instance_get_parameter_count = lib.func('remidy_instance_get_parameter_count',
    'int', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_get_parameter_info = lib.func('remidy_instance_get_parameter_info',
    'int', [koffi.pointer(RemidyPluginInstance), 'int', koffi.out(koffi.pointer(RemidyParameterInfo))]);
export const remidy_instance_get_parameter_value = lib.func('remidy_instance_get_parameter_value',
    'int', [koffi.pointer(RemidyPluginInstance), 'uint32_t', koffi.out(koffi.pointer('double'))]);
export const remidy_instance_set_parameter_value = lib.func('remidy_instance_set_parameter_value',
    'int', [koffi.pointer(RemidyPluginInstance), 'uint32_t', 'double']);

export const remidy_instance_has_ui = lib.func('remidy_instance_has_ui',
    'bool', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_create_ui = lib.func('remidy_instance_create_ui',
    'int', [koffi.pointer(RemidyPluginInstance), 'bool', 'uintptr_t',
        koffi.pointer(RemidyUIResizeCallback), koffi.pointer('void')]);
export const remidy_instance_destroy_ui = lib.func('remidy_instance_destroy_ui',
    'void', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_show_ui = lib.func('remidy_instance_show_ui',
    'int', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_hide_ui = lib.func('remidy_instance_hide_ui',
    'int', [koffi.pointer(RemidyPluginInstance)]);
export const remidy_instance_get_ui_size = lib.func('remidy_instance_get_ui_size',
    'int', [koffi.pointer(RemidyPluginInstance), koffi.out(koffi.pointer('uint32_t')), koffi.out(koffi.pointer('uint32_t'))]);
export const remidy_instance_set_ui_size = lib.func('remidy_instance_set_ui_size',
    'int', [koffi.pointer(RemidyPluginInstance), 'uint32_t', 'uint32_t']);
export const remidy_instance_can_ui_resize = lib.func('remidy_instance_can_ui_resize',
    'bool', [koffi.pointer(RemidyPluginInstance)]);

export const remidy_container_window_create = lib.func('remidy_container_window_create',
    koffi.pointer(RemidyContainerWindow),
    ['string', 'int', 'int', koffi.pointer(RemidyContainerWindowCloseCallback), koffi.pointer('void')]);
export const remidy_container_window_destroy = lib.func('remidy_container_window_destroy',
    'void', [koffi.pointer(RemidyContainerWindow)]);
export const remidy_container_window_show = lib.func('remidy_container_window_show',
    'void', [koffi.pointer(RemidyContainerWindow), 'bool']);
export const remidy_container_window_resize = lib.func('remidy_container_window_resize',
    'void', [koffi.pointer(RemidyContainerWindow), 'int', 'int']);
export const remidy_container_window_get_bounds = lib.func('remidy_container_window_get_bounds',
    RemidyBounds, [koffi.pointer(RemidyContainerWindow)]);
export const remidy_container_window_get_handle = lib.func('remidy_container_window_get_handle',
    'uintptr_t', [koffi.pointer(RemidyContainerWindow)]);
export const remidy_gl_context_guard_create = lib.func('remidy_gl_context_guard_create',
    koffi.pointer(RemidyGLContextGuard), []);
export const remidy_gl_context_guard_destroy = lib.func('remidy_gl_context_guard_destroy',
    'void', [koffi.pointer(RemidyGLContextGuard)]);

// ========== EventLoop API ==========

// Callback type for main thread tasks
const RemidyMainThreadTask = koffi.proto('void RemidyMainThreadTask(void* user_data)');

// Callback type for enqueuing tasks
const RemidyEnqueueCallback = koffi.proto('void RemidyEnqueueCallback(void* task, void* user_data, void* context)');

export const remidy_eventloop_init_nodejs = lib.func('remidy_eventloop_init_nodejs',
    'void', [koffi.pointer(RemidyEnqueueCallback), koffi.pointer('void')]);

export { RemidyConfigurationRequest, RemidyParameterInfo, RemidyPluginFormatInfo, RemidyBounds };
