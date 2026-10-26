// uapmd-api.js
// Public API for UAPMD - wraps the low-level __remidy_* functions
//
// IMPORTANT: The __remidy_* functions are internal implementation details
// and are NOT part of the stable API. Always use the uapmd.* API instead.

function unwrapTrackProperties(result) {
    if (result.error)
        throw new Error(result.error);
    return result;
}

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
        // Preset loading captures plug-in state asynchronously and returns a
        // MutationJob, just like other remote document mutations.
        return __remidy_instance_load_preset(this.instanceId, presetIndex);
    }
}

const __uapmdAutomationState = globalThis.__uapmdAutomationState ??= {
    jobs: Object.create(null)
};

function __uapmdClone(value) {
    return JSON.parse(JSON.stringify(value));
}

function __uapmdFindMatchingInstance(format, pluginId, trackId) {
    const tracks = __remidy_sequencer_getTrackInfos();
    const track = trackId ? tracks.find(t => t.trackId === trackId) : null;
    if (track) {
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
        const node = __uapmdFindMatchingInstance(job.format, job.pluginId, job.trackId);
        if (node) {
            job.state = "completed";
            job.instanceId = node.instanceId;
            job.completedAtMs = Date.now();
        }
    }

    return __uapmdClone(job);
}

globalThis.uapmd = {
    mutations: {
        getJob: (jobId) => __remidy_mutation_job_get(jobId),
        clearJob: (jobId) => __remidy_mutation_job_clear(jobId)
    },

    project: {
        save: (path) => __remidy_project_save(path),
        load: (path) => __remidy_project_load(path),
        loadHandle: (token) => __remidy_project_load_handle(token)
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

    // Instance creation and management. An empty trackId creates a new track.
    instancing: {
        create: (format, pluginId, trackId = "") => __remidy_instance_create(format, pluginId, trackId),
        startCreateJob: (jobId, format, pluginId, trackId = "") => {
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
                trackId,
                instanceId: -1,
                startedAtMs: Date.now()
            };
            __uapmdAutomationState.jobs[jobId] = job;

            try {
                const instanceId = __remidy_instance_create(format, pluginId, trackId);
                if (instanceId >= 0) {
                    job.state = "completed";
                    job.instanceId = instanceId;
                    job.completedAtMs = Date.now();
                } else {
                    const node = __uapmdFindMatchingInstance(format, pluginId, trackId);
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

    // Audio engine on/off. Disable before heavy operations (e.g. loading a large
    // project) so the realtime render thread does not compete for CPU; re-enable after.
    audio: {
        setEngineEnabled: (enabled) => __remidy_set_audio_engine_enabled(enabled),
        isEngineEnabled: () => __remidy_is_audio_engine_enabled()
    },

    // Sequencer API - Audio engine control and queries
    sequencer: {
        // MIDI control
        sendNoteOn: (instanceId, note) => __remidy_sequencer_sendNoteOn(instanceId, note),
        sendNoteOff: (instanceId, note) => __remidy_sequencer_sendNoteOff(instanceId, note),
        setParameterValue: (instanceId, paramIndex, value) => __remidy_sequencer_setParameterValue(instanceId, paramIndex, value),
        getHistoryState: () => __remidy_sequencer_get_history_state(),
        beginCompound: (description) => __remidy_sequencer_begin_compound(description),
        endCompound: () => __remidy_sequencer_end_compound(),
        cancelCompound: () => __remidy_sequencer_cancel_compound(),
        undo: () => __remidy_sequencer_undo(),
        redo: () => __remidy_sequencer_redo(),

        // Transport control
        startPlayback: () => __remidy_sequencer_startPlayback(),
        stopPlayback: () => __remidy_sequencer_stopPlayback(),
        pausePlayback: () => __remidy_sequencer_pausePlayback(),
        resumePlayback: () => __remidy_sequencer_resumePlayback(),
        jumpPlayback: (positionSeconds) => __remidy_sequencer_jumpPlayback(positionSeconds),
        getPlaybackPosition: () => __remidy_sequencer_getPlaybackPosition(),

        // Instance queries
        getInstanceIds: () => __remidy_sequencer_getInstanceIds(),
        getPluginName: (instanceId) => __remidy_sequencer_getPluginName(instanceId),
        getPluginFormat: (instanceId) => __remidy_sequencer_getPluginFormat(instanceId),
        isPluginBypassed: (instanceId) => __remidy_sequencer_isPluginBypassed(instanceId),
        setPluginBypassed: (instanceId, bypassed) => __remidy_sequencer_setPluginBypassed(instanceId, bypassed),
        getTrackInfos: () => __remidy_sequencer_getTrackInfos(),
        getTrackProperties: (trackId) => unwrapTrackProperties(__remidy_sequencer_getTrackProperties(trackId)),
        // Partial update; color is #RRGGBB or null (theme default), path is a
        // string, order is a finite number. Returns the updated properties.
        setTrackProperties: (trackId, properties) => unwrapTrackProperties(__remidy_sequencer_setTrackProperties(trackId, properties)),
        // Structural track mutations return a MutationJob. Poll it through
        // uapmd.mutations.getJob(jobId); the engine model thread is never
        // blocked waiting for asynchronous restoration or history capture.
        addTrack: () => __remidy_sequencer_add_track(),
        removeTrack: (trackId) => __remidy_sequencer_remove_track(trackId),
        clearTracks: () => __remidy_sequencer_clear_tracks(),
        getParameterUpdates: (instanceId) => __remidy_sequencer_getParameterUpdates(instanceId),
        consumeParameterMetadataRefresh: (instanceId) => __remidy_sequencer_consumeParameterMetadataRefresh(instanceId),

        // Audio analysis
        getInputTimeDomainData: (numSamples = 256) => __remidy_sequencer_getInputTimeDomainData(numSamples),
        getOutputTimeDomainData: (numSamples = 256) => __remidy_sequencer_getOutputTimeDomainData(numSamples),
        getInputFrequencyData: (numBins = 128) => __remidy_sequencer_getInputFrequencyData(numBins),
        getOutputFrequencyData: (numBins = 128) => __remidy_sequencer_getOutputFrequencyData(numBins),

        // Audio device settings
        getSampleRate: () => __remidy_sequencer_getSampleRate(),
        setSampleRate: (sampleRate) => __remidy_sequencer_setSampleRate(sampleRate),
        reconfigureAudioDevice: (inputIndex, outputIndex, sampleRate, bufferSize) =>
            __remidy_sequencer_reconfigureAudioDevice(inputIndex, outputIndex, sampleRate, bufferSize),
        isScanning: () => __remidy_sequencer_isScanning()
    },

    // Timeline API - tracks and clips are addressed by the project's string trackId/clipId.
    timeline: {
        getState: () => __remidy_timeline_get_state(),
        getMasterTrackId: () => __remidy_timeline_get_master_track_id(),
        setTempo: (bpm) => __remidy_timeline_set_tempo(bpm),
        getClips: (trackId) => __remidy_timeline_get_clips(trackId),
        getClipAudioEvents: (clipId) => __remidy_timeline_get_clip_audio_events(clipId),
        setClipAudioEvents: (clipId, payload) => __remidy_timeline_set_clip_audio_events(clipId, payload),
        getMasterMarkers: () => __remidy_timeline_get_master_markers(),
        setMasterMarkers: (markers) => __remidy_timeline_set_master_markers(markers),
        addMidiClip: (trackId, positionSamples, filepath) => __remidy_timeline_add_midi_clip(trackId, positionSamples, filepath),
        // Multi-track SMF import: creates one new track per SMF track, plus a master-track clip
        // (anchored to its source track's clip) for each track that carries its own tempo/
        // time-signature data. Mirrors the GUI's "Import MIDI Tracks (SMF)" menu action.
        importMidiTracks: (filepath) => __remidy_timeline_import_midi_tracks(filepath),
        removeClip: (clipId) => __remidy_timeline_remove_clip(clipId),
        createEmptyMidiClip: (trackId, positionSamples = 0, tickResolution = 480, bpm = 120.0) =>
            __remidy_timeline_create_empty_midi_clip(trackId, positionSamples, tickResolution, bpm),
        getLatencyCompensationState: () => __remidy_timeline_get_latency_compensation_state(),
        setLatencyCompensationState: (payload) => __remidy_timeline_set_latency_compensation_state(payload),
        getLatencyCompensationDebugState: () => __remidy_timeline_get_latency_compensation_debug_state(),
        setLatencyCompensationDebugState: (payload) => __remidy_timeline_set_latency_compensation_debug_state(payload),
        getClipUmpEvents: (clipId) => __remidy_timeline_get_clip_ump_events(clipId),
        // noteId/pointId are not saved; across addUmpEvent/removeUmpEvent they last while the note-on or point message is unchanged.
        getClipNotes: (clipId) => __remidy_timeline_get_clip_notes(clipId),
        addUmpEvent: (clipId, tick, words) => __remidy_timeline_add_ump_event(clipId, tick, words),
        removeUmpEvent: (clipId, eventIndex) => __remidy_timeline_remove_ump_event(clipId, eventIndex),
        ensureDagGraph: (trackId) => __remidy_timeline_ensure_dag_graph(trackId),
        showTrackGraph: (trackId) => __remidy_timeline_show_track_graph(trackId),
        revertTrackGraphToSimple: (trackId) => __remidy_timeline_revert_track_graph_to_simple(trackId),
        getTrackGraphConnections: (trackId) => __remidy_timeline_get_track_graph_connections(trackId),
        connectTrackGraph: (trackId, busType, source, target) => __remidy_timeline_connect_track_graph(
            trackId,
            busType,
            source.type,
            source.instanceId ?? -1,
            source.busIndex ?? 0,
            target.type,
            target.instanceId ?? -1,
            target.busIndex ?? 0,
            source.nodeId ?? "",
            target.nodeId ?? ""
        ),
        disconnectTrackGraphConnection: (trackId, connectionId) =>
            __remidy_timeline_disconnect_track_graph_connection(trackId, connectionId)
    },

    // Selection API - an omitted context means the active context, else the default (timeline) one.
    selection: {
        getContexts: () => __remidy_selection_get_contexts(),
        contextForClip: (clipId) => __remidy_selection_context_for_clip(clipId),
        get: (context) => __remidy_selection_get(context ?? -1),
        set: (selection, context) => __remidy_selection_set(selection, context ?? -1),
        clear: (context) => __remidy_selection_clear(context ?? -1),
        // null leaves no context active.
        setActiveContext: (context) => __remidy_selection_set_active_context(context ?? -1),
        getObject: (id) => __remidy_selection_get_object(id),
        getProjection: (context) => __remidy_selection_get_projection(context ?? -1)
    },

    render: {
        start: (outputPath, startSeconds = 0.0, endSeconds = -1.0, tailSeconds = 2.0, useContentFallback = true) =>
            __remidy_render_start(outputPath, startSeconds, endSeconds, tailSeconds, useContentFallback),
        getStatus: () => __remidy_render_get_status(),
        clearStatus: () => __remidy_render_clear_status(),
        cancel: () => __remidy_render_cancel()
    }
};
