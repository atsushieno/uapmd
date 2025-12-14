// Main entry point for @remidy/node package

// Initialize the EventLoop for Node.js (must happen before any plugin operations)
import './eventloop';

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
    type PluginUICreateOptions,
    type NativeHandle,
} from './plugin-instance';

export {
    ContainerWindow,
    type Bounds,
} from './container-window';

export {
    GLContextGuard,
} from './gl-context-guard';

// Re-export for convenience
export { PluginCatalog as Catalog } from './plugin-catalog';
export { PluginScanTool as ScanTool } from './plugin-scan-tool';
export { PluginInstance as Instance } from './plugin-instance';
