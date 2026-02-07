// uapmd-api.js
// Public API for UAPMD - wraps the low-level __remidy_* functions
//
// IMPORTANT: The __remidy_* functions are internal implementation details
// and are NOT part of the stable API. Always use the uapmd.* API instead.

// PluginInstance class - wraps instance control methods
class PluginInstance {
    constructor(instanceId) {
        this.instanceId = instanceId;
    }

    getParameters() {
        return __remidy_instance_get_parameters(this.instanceId);
    }

    getParameterValue(paramId) {
        return __remidy_instance_get_parameter_value(this.instanceId, paramId);
    }

    setParameterValue(paramId, value) {
        __remidy_instance_set_parameter_value(this.instanceId, paramId, value);
    }

    dispose() {
        __remidy_instance_dispose(this.instanceId);
    }

    configure(config) {
        __remidy_instance_configure(this.instanceId, config);
    }

    startProcessing() {
        __remidy_instance_start_processing(this.instanceId);
    }

    stopProcessing() {
        __remidy_instance_stop_processing(this.instanceId);
    }

    enableUmpDevice(deviceName) {
        __remidy_instance_enable_ump_device(this.instanceId, deviceName);
    }

    disableUmpDevice() {
        __remidy_instance_disable_ump_device(this.instanceId);
    }

    showUI() {
        __remidy_instance_show_ui(this.instanceId);
    }

    hideUI() {
        __remidy_instance_hide_ui(this.instanceId);
    }

    saveState(filepath) {
        return __remidy_instance_save_state(this.instanceId, filepath);
    }

    loadState(filepath) {
        return __remidy_instance_load_state(this.instanceId, filepath);
    }
}

globalThis.uapmd = {
    // Catalog API - Plugin discovery and management
    catalog: {
        getCount: () => __remidy_catalog_get_count(),
        getPluginAt: (index) => __remidy_catalog_get_plugin_at(index),
        save: (path) => __remidy_catalog_save(path)
    },

    // Scan Tool API - Plugin scanning and caching
    scanTool: {
        performScanning: () => __remidy_scan_tool_perform_scanning(),
        getFormats: () => __remidy_scan_tool_get_formats(),
        saveCache: (path) => __remidy_scan_tool_save_cache(path),
        setCacheFile: (path) => __remidy_scan_tool_set_cache_file(path)
    },

    // Instance creation and management
    instancing: {
        create: (format, pluginId, trackIndex = -1) => __remidy_instance_create(format, pluginId, trackIndex)
    },

    // Factory function to create PluginInstance wrapper for an existing instance
    instance: (instanceId) => new PluginInstance(instanceId),

    // Sequencer API - Audio engine control and queries
    sequencer: {
        // MIDI control
        sendNoteOn: (instanceId, note) => __remidy_sequencer_sendNoteOn(instanceId, note),
        sendNoteOff: (instanceId, note) => __remidy_sequencer_sendNoteOff(instanceId, note),
        setParameterValue: (instanceId, paramIndex, value) => __remidy_sequencer_setParameterValue(instanceId, paramIndex, value),

        // Transport control
        startPlayback: () => __remidy_sequencer_startPlayback(),
        stopPlayback: () => __remidy_sequencer_stopPlayback(),
        pausePlayback: () => __remidy_sequencer_pausePlayback(),
        resumePlayback: () => __remidy_sequencer_resumePlayback(),
        getPlaybackPosition: () => __remidy_sequencer_getPlaybackPosition(),

        // Instance queries
        getInstanceIds: () => __remidy_sequencer_getInstanceIds(),
        getPluginName: (instanceId) => __remidy_sequencer_getPluginName(instanceId),
        getPluginFormat: (instanceId) => __remidy_sequencer_getPluginFormat(instanceId),
        isPluginBypassed: (instanceId) => __remidy_sequencer_isPluginBypassed(instanceId),
        setPluginBypassed: (instanceId, bypassed) => __remidy_sequencer_setPluginBypassed(instanceId, bypassed),
        getTrackInfos: () => __remidy_sequencer_getTrackInfos(),
        addTrack: () => __remidy_sequencer_add_track(),
        removeTrack: (trackIndex) => __remidy_sequencer_remove_track(trackIndex),
        clearTracks: () => __remidy_sequencer_clear_tracks(),
        getParameterUpdates: (instanceId) => __remidy_sequencer_getParameterUpdates(instanceId),
        consumeParameterMetadataRefresh: (instanceId) => __remidy_sequencer_consumeParameterMetadataRefresh(instanceId),

        // Audio analysis
        getInputSpectrum: (numBars) => __remidy_sequencer_getInputSpectrum(numBars),
        getOutputSpectrum: (numBars) => __remidy_sequencer_getOutputSpectrum(numBars),

        // Audio device settings
        getSampleRate: () => __remidy_sequencer_getSampleRate(),
        setSampleRate: (sampleRate) => __remidy_sequencer_setSampleRate(sampleRate),
        isScanning: () => __remidy_sequencer_isScanning()
    }
};
