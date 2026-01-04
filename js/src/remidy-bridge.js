// remidy-bridge.js
// JavaScript bridge to C++ UAPMD API
// This module wraps the native C++ functions exposed via QuickJS

export class PluginCatalogEntry {
    constructor(data) {
        this.format = data.format || '';
        this.pluginId = data.pluginId || '';
        this.displayName = data.displayName || '';
        this.vendorName = data.vendorName || '';
        this.productUrl = data.productUrl || '';
        this.bundlePath = data.bundlePath || '';
    }
}

export class PluginCatalog {
    constructor() {
        // Catalog is managed by C++, we just provide the wrapper
    }

    get count() {
        return __remidy_catalog_get_count();
    }

    getPluginAt(index) {
        const data = __remidy_catalog_get_plugin_at(index);
        return data ? new PluginCatalogEntry(data) : null;
    }

    getPlugins() {
        const count = this.count;
        const plugins = [];
        for (let i = 0; i < count; i++) {
            const plugin = this.getPluginAt(i);
            if (plugin) {
                plugins.push(plugin);
            }
        }
        return plugins;
    }

    load(path) {
        return __remidy_catalog_load(path);
    }

    save(path) {
        return __remidy_catalog_save(path);
    }
}

export class PluginFormat {
    constructor(name) {
        this.name = name;
    }
}

export class PluginScanTool {
    constructor() {
        this._catalog = new PluginCatalog();
    }

    get catalog() {
        return this._catalog;
    }

    performScanning() {
        __remidy_scan_tool_perform_scanning();
    }

    getFormats() {
        const formatNames = __remidy_scan_tool_get_formats();
        return formatNames.map(name => new PluginFormat(name));
    }

    saveCache(path) {
        __remidy_scan_tool_save_cache(path);
    }

    setCacheFile(path) {
        __remidy_scan_tool_set_cache_file(path);
    }

    filterByFormat(entries, format) {
        return entries.filter(entry => entry.format === format);
    }
}

export class ParameterInfo {
    constructor(data) {
        this.id = data.id || 0;
        this.name = data.name || '';
        this.minValue = data.minValue || 0;
        this.maxValue = data.maxValue || 1;
        this.defaultValue = data.defaultValue || 0;
        this.isAutomatable = data.isAutomatable || false;
        this.isReadonly = data.isReadonly || false;
    }
}

export class PluginInstance {
    constructor(formatName, pluginId) {
        this.instanceId = __remidy_instance_create(formatName, pluginId);
        if (this.instanceId < 0) {
            throw new Error('Failed to create plugin instance');
        }
    }

    configure(config) {
        return __remidy_instance_configure(this.instanceId, config);
    }

    startProcessing() {
        __remidy_instance_start_processing(this.instanceId);
    }

    stopProcessing() {
        __remidy_instance_stop_processing(this.instanceId);
    }

    getParameters() {
        const params = __remidy_instance_get_parameters(this.instanceId);
        return params.map(p => new ParameterInfo(p));
    }

    getParameterValue(id) {
        return __remidy_instance_get_parameter_value(this.instanceId, id);
    }

    setParameterValue(id, value) {
        __remidy_instance_set_parameter_value(this.instanceId, id, value);
    }

    dispose() {
        if (this.instanceId >= 0) {
            __remidy_instance_dispose(this.instanceId);
            this.instanceId = -1;
        }
    }
}
