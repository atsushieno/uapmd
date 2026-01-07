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

    saveState(filepath) {
        return __remidy_instance_save_state(this.instanceId, filepath);
    }

    loadState(filepath) {
        return __remidy_instance_load_state(this.instanceId, filepath);
    }
}

export class ParameterUpdate {
    constructor(data) {
        this.parameterIndex = data.parameterIndex || 0;
        this.value = data.value || 0;
    }
}

export class PluginNodeInfo {
    constructor(data) {
        this.instanceId = data.instanceId || -1;
        this.pluginId = data.pluginId || '';
        this.format = data.format || '';
        this.displayName = data.displayName || '';
    }
}

export class TrackInfo {
    constructor(data) {
        this.trackIndex = data.trackIndex || -1;
        this.nodes = (data.nodes || []).map(n => new PluginNodeInfo(n));
    }
}

// Sequencer singleton - access to the application's audio sequencer
export const sequencer = {
    // MIDI Control
    sendNoteOn: function(instanceId, note) {
        __remidy_sequencer_sendNoteOn(instanceId, note);
    },

    sendNoteOff: function(instanceId, note) {
        __remidy_sequencer_sendNoteOff(instanceId, note);
    },

    setParameterValue: function(instanceId, parameterIndex, value) {
        __remidy_sequencer_setParameterValue(instanceId, parameterIndex, value);
    },

    // Transport/Playback
    startPlayback: function() {
        __remidy_sequencer_startPlayback();
    },

    stopPlayback: function() {
        __remidy_sequencer_stopPlayback();
    },

    pausePlayback: function() {
        __remidy_sequencer_pausePlayback();
    },

    resumePlayback: function() {
        __remidy_sequencer_resumePlayback();
    },

    getPlaybackPosition: function() {
        return __remidy_sequencer_getPlaybackPosition();
    },

    // Instance Management
    createPluginInstance: function(format, pluginId) {
        return __remidy_instance_create(format, pluginId);
    },

    enableUmpDevice: function(instanceId, deviceName) {
        __remidy_instance_enable_ump_device(instanceId, deviceName || "");
    },

    disableUmpDevice: function(instanceId) {
        __remidy_instance_disable_ump_device(instanceId);
    },

    showPluginUI: function(instanceId) {
        __remidy_instance_show_ui(instanceId);
    },

    hidePluginUI: function(instanceId) {
        __remidy_instance_hide_ui(instanceId);
    },

    getInstanceIds: function() {
        return __remidy_sequencer_getInstanceIds();
    },

    getPluginName: function(instanceId) {
        return __remidy_sequencer_getPluginName(instanceId);
    },

    getPluginFormat: function(instanceId) {
        return __remidy_sequencer_getPluginFormat(instanceId);
    },

    isPluginBypassed: function(instanceId) {
        return __remidy_sequencer_isPluginBypassed(instanceId);
    },

    setPluginBypassed: function(instanceId, bypassed) {
        __remidy_sequencer_setPluginBypassed(instanceId, bypassed);
    },

    getTrackInfos: function() {
        const tracks = __remidy_sequencer_getTrackInfos();
        return tracks.map(t => new TrackInfo(t));
    },

    getParameterUpdates: function(instanceId) {
        const updates = __remidy_sequencer_getParameterUpdates(instanceId);
        return updates.map(u => new ParameterUpdate(u));
    },

    // Audio Analysis
    getInputSpectrum: function(numBars) {
        if (numBars === undefined) numBars = 32;
        return __remidy_sequencer_getInputSpectrum(numBars);
    },

    getOutputSpectrum: function(numBars) {
        if (numBars === undefined) numBars = 32;
        return __remidy_sequencer_getOutputSpectrum(numBars);
    },

    // Audio Device/Settings
    getSampleRate: function() {
        return __remidy_sequencer_getSampleRate();
    },

    setSampleRate: function(sampleRate) {
        return __remidy_sequencer_setSampleRate(sampleRate);
    },

    isScanning: function() {
        return __remidy_sequencer_isScanning();
    },

    // Plugin State Management
    savePluginState: function(instanceId, filepath) {
        return __remidy_instance_save_state(instanceId, filepath);
    },

    loadPluginState: function(instanceId, filepath) {
        return __remidy_instance_load_state(instanceId, filepath);
    }
};
