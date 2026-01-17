// uapmd-api.js
// Public API for UAPMD - wraps the low-level __remidy_* functions
//
// IMPORTANT: The __remidy_* functions are internal implementation details
// and are NOT part of the stable API. Always use the uapmd.* API instead.

globalThis.uapmd = {
    // Catalog API - Plugin discovery and management
    catalog: {
        getCount: () => __remidy_catalog_get_count(),
        getPluginAt: (index) => __remidy_catalog_get_plugin_at(index),
        load: (path) => __remidy_catalog_load(path),
        save: (path) => __remidy_catalog_save(path)
    },

    // Scan Tool API - Plugin scanning and caching
    scanTool: {
        performScanning: () => __remidy_scan_tool_perform_scanning(),
        getFormats: () => __remidy_scan_tool_get_formats(),
        saveCache: (path) => __remidy_scan_tool_save_cache(path),
        setCacheFile: (path) => __remidy_scan_tool_set_cache_file(path)
    },

    // Instance API - Plugin instance lifecycle and control
    instance: {
        create: (format, pluginId) => __remidy_instance_create(format, pluginId),
        getParameters: (instanceId) => __remidy_instance_get_parameters(instanceId),
        getParameterValue: (instanceId, paramId) => __remidy_instance_get_parameter_value(instanceId, paramId),
        setParameterValue: (instanceId, paramId, value) => __remidy_instance_set_parameter_value(instanceId, paramId, value),
        dispose: (instanceId) => __remidy_instance_dispose(instanceId),
        configure: (instanceId, config) => __remidy_instance_configure(instanceId, config),
        startProcessing: (instanceId) => __remidy_instance_start_processing(instanceId),
        stopProcessing: (instanceId) => __remidy_instance_stop_processing(instanceId),
        enableUmpDevice: (instanceId, deviceName) => __remidy_instance_enable_ump_device(instanceId, deviceName),
        disableUmpDevice: (instanceId) => __remidy_instance_disable_ump_device(instanceId),
        showUI: (instanceId) => __remidy_instance_show_ui(instanceId),
        hideUI: (instanceId) => __remidy_instance_hide_ui(instanceId),
        saveState: (instanceId, filepath) => __remidy_instance_save_state(instanceId, filepath),
        loadState: (instanceId, filepath) => __remidy_instance_load_state(instanceId, filepath)
    },

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
        getParameterUpdates: (instanceId) => __remidy_sequencer_getParameterUpdates(instanceId),

        // Audio analysis
        getInputSpectrum: (numBars) => __remidy_sequencer_getInputSpectrum(numBars),
        getOutputSpectrum: (numBars) => __remidy_sequencer_getOutputSpectrum(numBars),

        // Audio device settings
        getSampleRate: () => __remidy_sequencer_getSampleRate(),
        setSampleRate: (sampleRate) => __remidy_sequencer_setSampleRate(sampleRate),
        isScanning: () => __remidy_sequencer_isScanning()
    }
};
