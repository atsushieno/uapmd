import * as koffi from 'koffi';
import * as path from 'path';
import * as os from 'os';

// Determine library name based on platform
function getLibraryPath(): string {
    const platform = os.platform();
    const ext = platform === 'darwin' ? 'dylib' : platform === 'win32' ? 'dll' : 'so';

    // Try different possible locations
    const possiblePaths = [
        path.join(__dirname, '..', 'dist', 'native', `libremidy_c.${ext}`),
        path.join(__dirname, '..', 'native', 'build', `libremidy_c.${ext}`),
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
export type PluginCatalogHandle = koffi.IKoffiRegisteredType;
export type PluginCatalogEntryHandle = koffi.IKoffiRegisteredType;
export type PluginFormatHandle = koffi.IKoffiRegisteredType;
export type PluginInstanceHandle = koffi.IKoffiRegisteredType;
export type PluginScanToolHandle = koffi.IKoffiRegisteredType;

// Define opaque pointer types
const RemidyPluginCatalog = koffi.opaque('RemidyPluginCatalog');
const RemidyPluginCatalogEntry = koffi.opaque('RemidyPluginCatalogEntry');
const RemidyPluginFormat = koffi.opaque('RemidyPluginFormat');
const RemidyPluginInstance = koffi.opaque('RemidyPluginInstance');
const RemidyPluginScanTool = koffi.opaque('RemidyPluginScanTool');

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

export const remidy_instance_create = lib.func('remidy_instance_create',
    koffi.pointer(RemidyPluginInstance),
    [koffi.pointer(RemidyPluginFormat), koffi.pointer(RemidyPluginCatalogEntry)]);
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

export { RemidyConfigurationRequest, RemidyParameterInfo, RemidyPluginFormatInfo };
