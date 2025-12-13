// Main entry point for @remidy/node package

export { StatusCode } from './ffi';

export {
    PluginCatalog,
    PluginCatalogEntry,
    type PluginCatalogEntryInfo,
} from './plugin-catalog';

export {
    PluginScanTool,
    PluginFormat,
    type PluginFormatInfo,
} from './plugin-scan-tool';

export {
    PluginInstance,
    type ConfigurationRequest,
    type ParameterInfo,
} from './plugin-instance';

// Re-export for convenience
export { PluginCatalog as Catalog } from './plugin-catalog';
export { PluginScanTool as ScanTool } from './plugin-scan-tool';
export { PluginInstance as Instance } from './plugin-instance';
