/* uapmd C API — implementation for the uapmd-engine module bindings */

#include "c-api/uapmd-c-engine.h"
#include "c-api-internal.h"
#include <uapmd-engine/uapmd-engine.hpp>
#include <cstring>
#include <string>
#include <vector>

/* ── Cast helpers ─────────────────────────────────────────────────────────── */

static uapmd::SequencerEngine*       E(uapmd_sequencer_engine_t h)   { return reinterpret_cast<uapmd::SequencerEngine*>(h); }
static uapmd::SequencerTrack*        ST(uapmd_sequencer_track_t h)   { return reinterpret_cast<uapmd::SequencerTrack*>(h); }
static uapmd::TimelineFacade*        TF(uapmd_timeline_facade_t h)   { return reinterpret_cast<uapmd::TimelineFacade*>(h); }
static uapmd::AudioIODevice*         AD(uapmd_audio_io_device_t h)   { return reinterpret_cast<uapmd::AudioIODevice*>(h); }
static uapmd::AudioIODeviceManager*  ADM(uapmd_audio_io_device_mgr_t h) { return reinterpret_cast<uapmd::AudioIODeviceManager*>(h); }
static uapmd::MidiIODevice*          MD(uapmd_midi_io_device_t h)    { return reinterpret_cast<uapmd::MidiIODevice*>(h); }
static uapmd::DeviceIODispatcher*    DIO(uapmd_device_io_dispatcher_t h) { return reinterpret_cast<uapmd::DeviceIODispatcher*>(h); }
static uapmd::RealtimeSequencer*     RS(uapmd_realtime_sequencer_t h) { return reinterpret_cast<uapmd::RealtimeSequencer*>(h); }

/* ── C ↔ C++ position helpers ────────────────────────────────────────────── */

static uapmd::TimelinePosition to_cpp(uapmd_timeline_position_t p) {
    uapmd::TimelinePosition pos;
    pos.samples = p.samples;
    pos.legacy_beats = p.legacy_beats;
    return pos;
}

static uapmd_timeline_position_t to_c(const uapmd::TimelinePosition& p) {
    return { p.samples, p.legacy_beats };
}

/* ── Ownership registry for engines ──────────────────────────────────────── */

static std::mutex s_engine_mutex;
static std::unordered_map<uapmd::SequencerEngine*, std::unique_ptr<uapmd::SequencerEngine>> s_owned_engines;

/* ── Static string storage for results that return const char* ───────────── */

static thread_local std::string tl_error;

/* ═══════════════════════════════════════════════════════════════════════════
 *  SequencerEngine
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_sequencer_engine_t uapmd_engine_create(int32_t sample_rate, uint32_t audio_buffer_size, uint32_t ump_buffer_size) {
    auto engine = uapmd::SequencerEngine::create(sample_rate, audio_buffer_size, ump_buffer_size);
    auto raw = engine.get();
    std::lock_guard lock(s_engine_mutex);
    s_owned_engines[raw] = std::move(engine);
    return reinterpret_cast<uapmd_sequencer_engine_t>(raw);
}

void uapmd_engine_destroy(uapmd_sequencer_engine_t engine) {
    if (!engine) return;
    std::lock_guard lock(s_engine_mutex);
    s_owned_engines.erase(E(engine));
}

void uapmd_engine_enqueue_ump(uapmd_sequencer_engine_t engine, int32_t instance_id,
                               uapmd_ump_t* ump, size_t size_in_bytes, uapmd_timestamp_t timestamp) {
    E(engine)->enqueueUmp(instance_id, ump, size_in_bytes, timestamp);
}

uapmd_plugin_host_t uapmd_engine_plugin_host(uapmd_sequencer_engine_t engine) {
    return reinterpret_cast<uapmd_plugin_host_t>(E(engine)->pluginHost());
}

uapmd_plugin_instance_t uapmd_engine_get_plugin_instance(uapmd_sequencer_engine_t engine, int32_t instance_id) {
    return reinterpret_cast<uapmd_plugin_instance_t>(E(engine)->getPluginInstance(instance_id));
}

uapmd_function_block_mgr_t uapmd_engine_function_block_manager(uapmd_sequencer_engine_t engine) {
    return reinterpret_cast<uapmd_function_block_mgr_t>(E(engine)->functionBlockManager());
}

/* Track management */

uint32_t uapmd_engine_track_count(uapmd_sequencer_engine_t engine) {
    return static_cast<uint32_t>(E(engine)->tracks().size());
}

uapmd_sequencer_track_t uapmd_engine_get_track(uapmd_sequencer_engine_t engine, uint32_t index) {
    auto& tracks = E(engine)->tracks();
    if (index >= tracks.size()) return nullptr;
    return reinterpret_cast<uapmd_sequencer_track_t>(tracks[index]);
}

uapmd_sequencer_track_t uapmd_engine_master_track(uapmd_sequencer_engine_t engine) {
    return reinterpret_cast<uapmd_sequencer_track_t>(E(engine)->masterTrack());
}

int32_t uapmd_engine_add_empty_track(uapmd_sequencer_engine_t engine) {
    return E(engine)->addEmptyTrack();
}

void uapmd_engine_add_plugin_to_track(uapmd_sequencer_engine_t engine,
                                       int32_t track_index,
                                       const char* format,
                                       const char* plugin_id,
                                       void* user_data,
                                       uapmd_add_plugin_cb_t callback) {
    std::string fmt = format;
    std::string pid = plugin_id;
    E(engine)->addPluginToTrack(track_index, fmt, pid,
        [callback, user_data](int32_t instanceId, int32_t trackIdx, std::string error) {
            if (callback)
                callback(instanceId, trackIdx, error.empty() ? nullptr : error.c_str(), user_data);
        });
}

bool uapmd_engine_remove_plugin_instance(uapmd_sequencer_engine_t engine, int32_t instance_id) {
    return E(engine)->removePluginInstance(instance_id);
}

bool uapmd_engine_remove_track(uapmd_sequencer_engine_t engine, int32_t track_index) {
    return E(engine)->removeTrack(track_index);
}

void uapmd_engine_cleanup_empty_tracks(uapmd_sequencer_engine_t engine) {
    E(engine)->cleanupEmptyTracks();
}

int32_t uapmd_engine_find_track_for_instance(uapmd_sequencer_engine_t engine, int32_t instance_id) {
    return E(engine)->findTrackIndexForInstance(instance_id);
}

uint8_t uapmd_engine_get_instance_group(uapmd_sequencer_engine_t engine, int32_t instance_id) {
    return E(engine)->getInstanceGroup(instance_id);
}

bool uapmd_engine_set_instance_group(uapmd_sequencer_engine_t engine, int32_t instance_id, uint8_t group) {
    return E(engine)->setInstanceGroup(instance_id, group);
}

/* Latency */

uint32_t uapmd_engine_track_latency(uapmd_sequencer_engine_t engine, int32_t track_index)     { return E(engine)->trackLatencyInSamples(track_index); }
uint32_t uapmd_engine_master_track_latency(uapmd_sequencer_engine_t engine)                    { return E(engine)->masterTrackLatencyInSamples(); }
uint32_t uapmd_engine_track_render_lead(uapmd_sequencer_engine_t engine, int32_t track_index)  { return E(engine)->trackRenderLeadInSamples(track_index); }
uint32_t uapmd_engine_master_track_render_lead(uapmd_sequencer_engine_t engine)                { return E(engine)->masterTrackRenderLeadInSamples(); }

/* Configuration */

void uapmd_engine_set_default_channels(uapmd_sequencer_engine_t engine, uint32_t in_ch, uint32_t out_ch) { E(engine)->setDefaultChannels(in_ch, out_ch); }
void uapmd_engine_set_sample_rate(uapmd_sequencer_engine_t engine, int32_t sample_rate)                  { E(engine)->setSampleRate(sample_rate); }
bool uapmd_engine_get_offline_rendering(uapmd_sequencer_engine_t engine)          { return E(engine)->offlineRendering(); }
void uapmd_engine_set_offline_rendering(uapmd_sequencer_engine_t engine, bool en) { E(engine)->offlineRendering(en); }
void uapmd_engine_set_active(uapmd_sequencer_engine_t engine, bool active)        { E(engine)->setEngineActive(active); }
void uapmd_engine_set_external_pump(uapmd_sequencer_engine_t engine, bool en)     { E(engine)->setExternalPump(en); }

/* Playback */

bool    uapmd_engine_is_playback_active(uapmd_sequencer_engine_t engine)        { return E(engine)->isPlaybackActive(); }
int64_t uapmd_engine_get_playback_position(uapmd_sequencer_engine_t engine)     { return E(engine)->playbackPosition(); }
void    uapmd_engine_set_playback_position(uapmd_sequencer_engine_t engine, int64_t s) { E(engine)->playbackPosition(s); }
int64_t uapmd_engine_render_playback_position(uapmd_sequencer_engine_t engine)  { return E(engine)->renderPlaybackPosition(); }
void    uapmd_engine_start_playback(uapmd_sequencer_engine_t engine)            { E(engine)->startPlayback(); }
void    uapmd_engine_stop_playback(uapmd_sequencer_engine_t engine)             { E(engine)->stopPlayback(); }
void    uapmd_engine_pause_playback(uapmd_sequencer_engine_t engine)            { E(engine)->pausePlayback(); }
void    uapmd_engine_resume_playback(uapmd_sequencer_engine_t engine)           { E(engine)->resumePlayback(); }

/* Convenience MIDI */

void uapmd_engine_send_note_on(uapmd_sequencer_engine_t e, int32_t id, int32_t note)         { E(e)->sendNoteOn(id, note); }
void uapmd_engine_send_note_off(uapmd_sequencer_engine_t e, int32_t id, int32_t note)        { E(e)->sendNoteOff(id, note); }
void uapmd_engine_send_pitch_bend(uapmd_sequencer_engine_t e, int32_t id, float v)           { E(e)->sendPitchBend(id, v); }
void uapmd_engine_send_channel_pressure(uapmd_sequencer_engine_t e, int32_t id, float p)     { E(e)->sendChannelPressure(id, p); }
void uapmd_engine_set_parameter_value(uapmd_sequencer_engine_t e, int32_t id, int32_t idx, double v) { E(e)->setParameterValue(id, idx, v); }

/* Audio analysis */

void uapmd_engine_get_input_spectrum(uapmd_sequencer_engine_t e, float* out, int n)  { E(e)->getInputSpectrum(out, n); }
void uapmd_engine_get_output_spectrum(uapmd_sequencer_engine_t e, float* out, int n) { E(e)->getOutputSpectrum(out, n); }

/* Timeline */

uapmd_timeline_facade_t uapmd_engine_timeline(uapmd_sequencer_engine_t engine) {
    return reinterpret_cast<uapmd_timeline_facade_t>(&E(engine)->timeline());
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SequencerTrack
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_plugin_graph_t uapmd_track_graph(uapmd_sequencer_track_t track) {
    return reinterpret_cast<uapmd_plugin_graph_t>(&ST(track)->graph());
}

uint32_t uapmd_track_latency_in_samples(uapmd_sequencer_track_t track)  { return ST(track)->latencyInSamples(); }
uint32_t uapmd_track_render_lead_in_samples(uapmd_sequencer_track_t track) { return ST(track)->renderLeadInSamples(); }
double   uapmd_track_tail_length_in_seconds(uapmd_sequencer_track_t track) { return ST(track)->tailLengthInSeconds(); }

bool uapmd_track_get_bypassed(uapmd_sequencer_track_t track) { return ST(track)->bypassed(); }
bool uapmd_track_get_frozen(uapmd_sequencer_track_t track)   { return ST(track)->frozen(); }
void uapmd_track_set_bypassed(uapmd_sequencer_track_t track, bool v) { ST(track)->bypassed(v); }
void uapmd_track_set_frozen(uapmd_sequencer_track_t track, bool v)   { ST(track)->frozen(v); }

uint32_t uapmd_track_ordered_instance_id_count(uapmd_sequencer_track_t track) {
    return static_cast<uint32_t>(ST(track)->orderedInstanceIds().size());
}

bool uapmd_track_get_ordered_instance_ids(uapmd_sequencer_track_t track, int32_t* out, uint32_t out_count) {
    auto& ids = ST(track)->orderedInstanceIds();
    if (out_count < ids.size()) return false;
    std::memcpy(out, ids.data(), ids.size() * sizeof(int32_t));
    return true;
}

void    uapmd_track_set_instance_group(uapmd_sequencer_track_t track, int32_t id, uint8_t g)  { ST(track)->setInstanceGroup(id, g); }
uint8_t uapmd_track_get_instance_group(uapmd_sequencer_track_t track, int32_t id)             { return ST(track)->getInstanceGroup(id); }
uint8_t uapmd_track_find_available_group(uapmd_sequencer_track_t track)                       { return ST(track)->findAvailableGroup(); }
void    uapmd_track_remove_instance(uapmd_sequencer_track_t track, int32_t instance_id)       { ST(track)->removeInstance(instance_id); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  TimelineFacade
 * ═══════════════════════════════════════════════════════════════════════════ */

bool uapmd_tl_get_state(uapmd_timeline_facade_t tl, uapmd_timeline_state_t* out) {
    auto& st = TF(tl)->state();
    out->playhead_position = to_c(st.playheadPosition);
    out->is_playing = st.isPlaying;
    out->loop_enabled = st.loopEnabled;
    out->loop_start = to_c(st.loopStart);
    out->loop_end = to_c(st.loopEnd);
    out->tempo = st.tempo;
    out->time_signature_numerator = st.timeSignatureNumerator;
    out->time_signature_denominator = st.timeSignatureDenominator;
    out->sample_rate = st.sample_rate;
    return true;
}

void uapmd_tl_set_tempo(uapmd_timeline_facade_t tl, double tempo) {
    TF(tl)->state().tempo = tempo;
}

void uapmd_tl_set_time_signature(uapmd_timeline_facade_t tl, int32_t num, int32_t den) {
    TF(tl)->state().timeSignatureNumerator = num;
    TF(tl)->state().timeSignatureDenominator = den;
}

void uapmd_tl_set_loop(uapmd_timeline_facade_t tl, bool enabled, uapmd_timeline_position_t start, uapmd_timeline_position_t end) {
    auto& st = TF(tl)->state();
    st.loopEnabled = enabled;
    st.loopStart = to_cpp(start);
    st.loopEnd = to_cpp(end);
}

uint32_t uapmd_tl_track_count(uapmd_timeline_facade_t tl) {
    return static_cast<uint32_t>(TF(tl)->tracks().size());
}

uapmd_timeline_track_t uapmd_tl_get_track(uapmd_timeline_facade_t tl, uint32_t index) {
    auto tracks = TF(tl)->tracks();
    if (index >= tracks.size()) return nullptr;
    return reinterpret_cast<uapmd_timeline_track_t>(tracks[index]);
}

uapmd_timeline_track_t uapmd_tl_master_timeline_track(uapmd_timeline_facade_t tl) {
    return reinterpret_cast<uapmd_timeline_track_t>(TF(tl)->masterTimelineTrack());
}

/* ── Clip add helpers ────────────────────────────────────────────────────── */

static uapmd_clip_add_result_t to_c_clip_result(const uapmd::TimelineFacade::ClipAddResult& r) {
    tl_error = r.error;
    return { r.clipId, r.sourceNodeId, r.success, tl_error.empty() ? nullptr : tl_error.c_str() };
}

uapmd_clip_add_result_t uapmd_tl_add_audio_clip(uapmd_timeline_facade_t tl,
                                                  int32_t track_index,
                                                  uapmd_timeline_position_t position,
                                                  uapmd_audio_file_reader_t reader,
                                                  const char* filepath) {
    /* Transfer ownership of the reader from the C registry to the engine */

    auto* raw = reinterpret_cast<uapmd::AudioFileReader*>(reader);
    std::unique_ptr<uapmd::AudioFileReader> owned;
    {
        std::lock_guard lock(s_reader_mutex);
        auto it = s_owned_readers.find(raw);
        if (it != s_owned_readers.end()) {
            owned = std::move(it->second);
            s_owned_readers.erase(it);
        }
    }
    if (!owned)
        return { -1, -1, false, "reader not found or already consumed" };

    auto r = TF(tl)->addAudioClipToTrack(track_index, to_cpp(position), std::move(owned), filepath ? filepath : "");
    return to_c_clip_result(r);
}

uapmd_clip_add_result_t uapmd_tl_add_midi_clip_from_file(uapmd_timeline_facade_t tl,
                                                          int32_t track_index,
                                                          uapmd_timeline_position_t position,
                                                          const char* filepath,
                                                          bool nrpn_to_parameter_mapping) {
    auto r = TF(tl)->addMidiClipToTrack(track_index, to_cpp(position), filepath, nrpn_to_parameter_mapping);
    return to_c_clip_result(r);
}

uapmd_clip_add_result_t uapmd_tl_add_midi_clip_from_data(uapmd_timeline_facade_t tl,
                                                          int32_t track_index,
                                                          uapmd_timeline_position_t position,
                                                          const uapmd_ump_t* ump_events,
                                                          uint32_t ump_event_count,
                                                          const uint64_t* tick_timestamps,
                                                          uint32_t tick_count,
                                                          uint32_t tick_resolution,
                                                          double clip_tempo,
                                                          const uapmd_midi_tempo_change_t* tempo_changes,
                                                          uint32_t tempo_change_count,
                                                          const uapmd_midi_time_sig_change_t* time_sig_changes,
                                                          uint32_t time_sig_change_count,
                                                          const char* clip_name,
                                                          bool nrpn_to_parameter_mapping,
                                                          bool needs_file_save) {
    std::vector<uapmd_ump_t> ump(ump_events, ump_events + ump_event_count);
    std::vector<uint64_t> ticks(tick_timestamps, tick_timestamps + tick_count);
    std::vector<uapmd::MidiTempoChange> tc(tempo_change_count);
    for (uint32_t i = 0; i < tempo_change_count; ++i) {
        tc[i].tickPosition = tempo_changes[i].tick_position;
        tc[i].bpm = tempo_changes[i].bpm;
    }
    std::vector<uapmd::MidiTimeSignatureChange> tsc(time_sig_change_count);
    for (uint32_t i = 0; i < time_sig_change_count; ++i) {
        tsc[i].tickPosition = time_sig_changes[i].tick_position;
        tsc[i].numerator = time_sig_changes[i].numerator;
        tsc[i].denominator = time_sig_changes[i].denominator;
        tsc[i].clocksPerClick = time_sig_changes[i].clocks_per_click;
        tsc[i].thirtySecondsPerQuarter = time_sig_changes[i].thirty_seconds_per_quarter;
    }

    auto r = TF(tl)->addMidiClipToTrack(track_index, to_cpp(position),
        std::move(ump), std::move(ticks), tick_resolution, clip_tempo,
        std::move(tc), std::move(tsc),
        clip_name ? clip_name : "", nrpn_to_parameter_mapping, needs_file_save);
    return to_c_clip_result(r);
}

bool uapmd_tl_remove_clip(uapmd_timeline_facade_t tl, int32_t track_index, int32_t clip_id) {
    return TF(tl)->removeClipFromTrack(track_index, clip_id);
}

uapmd_project_result_t uapmd_tl_load_project(uapmd_timeline_facade_t tl, const char* file_path) {
    auto r = TF(tl)->loadProject(file_path);
    tl_error = r.error;
    return { r.success, tl_error.empty() ? nullptr : tl_error.c_str() };
}

uapmd_content_bounds_t uapmd_tl_calculate_content_bounds(uapmd_timeline_facade_t tl) {
    auto b = TF(tl)->calculateContentBounds();
    return { b.hasContent, b.firstSample, b.lastSample, b.firstSeconds, b.lastSeconds };
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioIODeviceManager
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_audio_io_device_mgr_t uapmd_audio_device_mgr_instance(const char* driver_name) {
    return reinterpret_cast<uapmd_audio_io_device_mgr_t>(uapmd::AudioIODeviceManager::instance(driver_name ? driver_name : ""));
}

static thread_local std::vector<uapmd::AudioIODeviceInfo> tl_devices;

uint32_t uapmd_audio_device_mgr_device_count(uapmd_audio_io_device_mgr_t mgr) {
    tl_devices = ADM(mgr)->devices();
    return static_cast<uint32_t>(tl_devices.size());
}

bool uapmd_audio_device_mgr_get_device_info(uapmd_audio_io_device_mgr_t mgr, uint32_t index, uapmd_audio_device_info_t* out) {
    if (tl_devices.empty())
        tl_devices = ADM(mgr)->devices();
    if (index >= tl_devices.size()) return false;
    auto& d = tl_devices[index];
    out->directions = static_cast<uapmd_audio_io_direction_t>(d.directions);
    out->id = d.id;
    out->name = d.name.c_str();
    out->sample_rate = d.sampleRate;
    out->channels = d.channels;
    return true;
}

uapmd_audio_io_device_t uapmd_audio_device_mgr_open(uapmd_audio_io_device_mgr_t mgr,
                                                      int input_device_index,
                                                      int output_device_index,
                                                      uint32_t sample_rate,
                                                      uint32_t buffer_size) {
    return reinterpret_cast<uapmd_audio_io_device_t>(ADM(mgr)->open(input_device_index, output_device_index, sample_rate, buffer_size));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioIODevice
 * ═══════════════════════════════════════════════════════════════════════════ */

double   uapmd_audio_device_sample_rate(uapmd_audio_io_device_t dev)      { return AD(dev)->sampleRate(); }
uint32_t uapmd_audio_device_channels(uapmd_audio_io_device_t dev)         { return AD(dev)->channels(); }
uint32_t uapmd_audio_device_input_channels(uapmd_audio_io_device_t dev)   { return AD(dev)->inputChannels(); }
uint32_t uapmd_audio_device_output_channels(uapmd_audio_io_device_t dev)  { return AD(dev)->outputChannels(); }
uapmd_status_t uapmd_audio_device_start(uapmd_audio_io_device_t dev)      { return AD(dev)->start(); }
uapmd_status_t uapmd_audio_device_stop(uapmd_audio_io_device_t dev)       { return AD(dev)->stop(); }
bool     uapmd_audio_device_is_playing(uapmd_audio_io_device_t dev)       { return AD(dev)->isPlaying(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiIODevice
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_midi_io_device_t uapmd_midi_device_instance(const char* driver_name) {
    return reinterpret_cast<uapmd_midi_io_device_t>(uapmd::MidiIODevice::instance(driver_name ? driver_name : ""));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DeviceIODispatcher
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_device_io_dispatcher_t uapmd_default_device_io_dispatcher() {
    return reinterpret_cast<uapmd_device_io_dispatcher_t>(uapmd::defaultDeviceIODispatcher());
}

uapmd_status_t uapmd_dispatcher_start(uapmd_device_io_dispatcher_t disp)       { return DIO(disp)->start(); }
uapmd_status_t uapmd_dispatcher_stop(uapmd_device_io_dispatcher_t disp)        { return DIO(disp)->stop(); }
bool uapmd_dispatcher_is_playing(uapmd_device_io_dispatcher_t disp)            { return DIO(disp)->isPlaying(); }
void uapmd_dispatcher_clear_output_buffers(uapmd_device_io_dispatcher_t disp)  { DIO(disp)->clearOutputBuffers(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  RealtimeSequencer
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::mutex s_rt_seq_mutex;
static std::unordered_map<uapmd::RealtimeSequencer*, std::unique_ptr<uapmd::RealtimeSequencer>> s_owned_rt_seqs;

uapmd_realtime_sequencer_t uapmd_rt_sequencer_create(uint32_t buffer_size, uint32_t ump_buffer_size,
                                                       int32_t sample_rate, uapmd_device_io_dispatcher_t dispatcher) {
    auto seq = std::make_unique<uapmd::RealtimeSequencer>(buffer_size, ump_buffer_size, sample_rate, DIO(dispatcher));
    auto raw = seq.get();
    std::lock_guard lock(s_rt_seq_mutex);
    s_owned_rt_seqs[raw] = std::move(seq);
    return reinterpret_cast<uapmd_realtime_sequencer_t>(raw);
}

void uapmd_rt_sequencer_destroy(uapmd_realtime_sequencer_t seq) {
    if (!seq) return;
    std::lock_guard lock(s_rt_seq_mutex);
    s_owned_rt_seqs.erase(RS(seq));
}

uapmd_sequencer_engine_t uapmd_rt_sequencer_engine(uapmd_realtime_sequencer_t seq) {
    return reinterpret_cast<uapmd_sequencer_engine_t>(RS(seq)->engine());
}

uapmd_status_t uapmd_rt_sequencer_start_audio(uapmd_realtime_sequencer_t seq)   { return RS(seq)->startAudio(); }
uapmd_status_t uapmd_rt_sequencer_stop_audio(uapmd_realtime_sequencer_t seq)    { return RS(seq)->stopAudio(); }
uapmd_status_t uapmd_rt_sequencer_is_audio_playing(uapmd_realtime_sequencer_t seq) { return RS(seq)->isAudioPlaying(); }
void uapmd_rt_sequencer_clear_output_buffers(uapmd_realtime_sequencer_t seq)    { RS(seq)->clearOutputBuffers(); }
int32_t uapmd_rt_sequencer_sample_rate(uapmd_realtime_sequencer_t seq)          { return RS(seq)->sampleRate(); }

bool uapmd_rt_sequencer_set_sample_rate(uapmd_realtime_sequencer_t seq, int32_t new_sr) {
    return RS(seq)->sampleRate(new_sr);
}

bool uapmd_rt_sequencer_reconfigure_audio_device(uapmd_realtime_sequencer_t seq,
                                                   int input_device_index,
                                                   int output_device_index,
                                                   uint32_t sample_rate,
                                                   uint32_t buffer_size) {
    return RS(seq)->reconfigureAudioDevice(input_device_index, output_device_index, sample_rate, buffer_size);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Offline Renderer
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_offline_render_result_t uapmd_render_offline(uapmd_sequencer_engine_t engine,
                                                     const uapmd_offline_render_settings_t* settings,
                                                     void* user_data,
                                                     uapmd_render_progress_cb_t progress_cb,
                                                     uapmd_render_should_cancel_cb_t cancel_cb) {
    uapmd::OfflineRenderSettings s;
    s.outputPath = settings->output_path;
    s.startSeconds = settings->start_seconds;
    if (settings->has_end_seconds)
        s.endSeconds = settings->end_seconds;
    s.useContentFallback = settings->use_content_fallback;
    s.contentBoundsValid = settings->content_bounds_valid;
    s.contentStartSeconds = settings->content_start_seconds;
    s.contentEndSeconds = settings->content_end_seconds;
    s.tailSeconds = settings->tail_seconds;
    s.enableSilenceStop = settings->enable_silence_stop;
    s.silenceDurationSeconds = settings->silence_duration_seconds;
    s.silenceThresholdDb = settings->silence_threshold_db;
    s.sampleRate = settings->sample_rate;
    s.bufferSize = settings->buffer_size;
    s.outputChannels = settings->output_channels;
    s.umpBufferSize = settings->ump_buffer_size;

    uapmd::OfflineRenderCallbacks cb;
    if (progress_cb)
        cb.onProgress = [progress_cb, user_data](const uapmd::OfflineRenderProgress& p) {
            uapmd_offline_render_progress_t cp = { p.progress, p.renderedSeconds, p.totalSeconds, p.renderedFrames, p.totalFrames };
            progress_cb(&cp, user_data);
        };
    if (cancel_cb)
        cb.shouldCancel = [cancel_cb, user_data]() { return cancel_cb(user_data); };

    auto r = uapmd::renderOfflineProject(*E(engine), s, cb);
    tl_error = r.errorMessage;
    return { r.success, r.canceled, r.renderedSeconds, tl_error.empty() ? nullptr : tl_error.c_str() };
}
