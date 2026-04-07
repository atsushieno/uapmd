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

    getPresets() {
        return __remidy_instance_get_presets(this.instanceId);
    }

    loadPreset(presetIndex) {
        return __remidy_instance_load_preset(this.instanceId, presetIndex);
    }
}

const __uapmdAutomationState = globalThis.__uapmdAutomationState ??= {
    jobs: Object.create(null)
};

function __uapmdClone(value) {
    return JSON.parse(JSON.stringify(value));
}

function __uapmdFindMatchingInstance(format, pluginId, trackIndex) {
    const tracks = __remidy_sequencer_getTrackInfos();
    if (trackIndex >= 0 && trackIndex < tracks.length) {
        const track = tracks[trackIndex];
        const node = (track.nodes || []).find(n => n.format === format && n.pluginId === pluginId);
        if (node)
            return node;
    }

    for (const track of tracks) {
        const node = (track.nodes || []).find(n => n.format === format && n.pluginId === pluginId);
        if (node)
            return node;
    }

    return null;
}

function __uapmdGetCreateJob(jobId) {
    return __uapmdAutomationState.jobs[jobId] ?? null;
}

function __uapmdPollCreateJob(jobId) {
    const job = __uapmdGetCreateJob(jobId);
    if (!job)
        return null;

    if (job.state === "running") {
        const node = __uapmdFindMatchingInstance(job.format, job.pluginId, job.trackIndex);
        if (node) {
            job.state = "completed";
            job.instanceId = node.instanceId;
            job.completedAtMs = Date.now();
        }
    }

    return __uapmdClone(job);
}

globalThis.uapmd = {
    project: {
        save: (path) => __remidy_project_save(path),
        load: (path) => __remidy_project_load(path)
    },

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
        create: (format, pluginId, trackIndex = -1) => __remidy_instance_create(format, pluginId, trackIndex),
        startCreateJob: (jobId, format, pluginId, trackIndex = -1) => {
            if (!jobId)
                throw new Error("jobId is required");

            const existing = __uapmdPollCreateJob(jobId);
            if (existing)
                return existing;

            const job = {
                jobId,
                state: "running",
                format,
                pluginId,
                trackIndex,
                instanceId: -1,
                startedAtMs: Date.now()
            };
            __uapmdAutomationState.jobs[jobId] = job;

            try {
                const instanceId = __remidy_instance_create(format, pluginId, trackIndex);
                if (instanceId >= 0) {
                    job.state = "completed";
                    job.instanceId = instanceId;
                    job.completedAtMs = Date.now();
                } else {
                    const node = __uapmdFindMatchingInstance(format, pluginId, trackIndex);
                    if (node) {
                        job.state = "completed";
                        job.instanceId = node.instanceId;
                        job.completedAtMs = Date.now();
                    }
                }
            } catch (e) {
                job.state = "failed";
                job.error = String(e);
                job.completedAtMs = Date.now();
            }

            return __uapmdClone(job);
        },
        getCreateJob: (jobId) => __uapmdPollCreateJob(jobId),
        clearCreateJob: (jobId) => {
            delete __uapmdAutomationState.jobs[jobId];
        }
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
    },

    // Timeline API - Clip and playback state management
    timeline: {
        getState: () => __remidy_timeline_get_state(),
        setTempo: (bpm) => __remidy_timeline_set_tempo(bpm),
        getClips: (trackIndex) => __remidy_timeline_get_clips(trackIndex),
        getClipAudioEvents: (trackIndex, clipId) => __remidy_timeline_get_clip_audio_events(trackIndex, clipId),
        setClipAudioEvents: (trackIndex, clipId, payload) => __remidy_timeline_set_clip_audio_events(trackIndex, clipId, payload),
        getMasterMarkers: () => __remidy_timeline_get_master_markers(),
        setMasterMarkers: (markers) => __remidy_timeline_set_master_markers(markers),
        addMidiClip: (trackIndex, positionSamples, filepath) => __remidy_timeline_add_midi_clip(trackIndex, positionSamples, filepath),
        removeClip: (trackIndex, clipId) => __remidy_timeline_remove_clip(trackIndex, clipId),
        createEmptyMidiClip: (trackIndex, positionSamples = 0, tickResolution = 480, bpm = 120.0) =>
            __remidy_timeline_create_empty_midi_clip(trackIndex, positionSamples, tickResolution, bpm),
        getClipUmpEvents: (trackIndex, clipId) => __remidy_timeline_get_clip_ump_events(trackIndex, clipId),
        addUmpEvent: (trackIndex, clipId, tick, words) => __remidy_timeline_add_ump_event(trackIndex, clipId, tick, words),
        removeUmpEvent: (trackIndex, clipId, eventIndex) => __remidy_timeline_remove_ump_event(trackIndex, clipId, eventIndex)
    },

    render: {
        start: (outputPath, startSeconds = 0.0, endSeconds = -1.0, tailSeconds = 2.0, useContentFallback = true) =>
            __remidy_render_start(outputPath, startSeconds, endSeconds, tailSeconds, useContentFallback),
        getStatus: () => __remidy_render_get_status(),
        clearStatus: () => __remidy_render_clear_status(),
        cancel: () => __remidy_render_cancel()
    }
};
