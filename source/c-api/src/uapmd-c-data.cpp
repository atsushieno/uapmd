/* uapmd C API — implementation for the uapmd-data module bindings */

#include "c-api/uapmd-c-data.h"
#include <uapmd-data/uapmd-data.hpp>
#include <cstring>
#include <string>
#include <vector>

/* ── Cast helpers ─────────────────────────────────────────────────────────── */

static uapmd::ClipManager*          CM(uapmd_clip_manager_t h)       { return reinterpret_cast<uapmd::ClipManager*>(h); }
static uapmd::TimelineTrack*        TT(uapmd_timeline_track_t h)    { return reinterpret_cast<uapmd::TimelineTrack*>(h); }
static uapmd::SourceNode*           SN(uapmd_source_node_t h)       { return reinterpret_cast<uapmd::SourceNode*>(h); }
static uapmd::AudioSourceNode*      ASN(uapmd_audio_source_node_t h) { return reinterpret_cast<uapmd::AudioSourceNode*>(h); }
static uapmd::AudioFileSourceNode*  AFSN(uapmd_audio_file_source_t h) { return reinterpret_cast<uapmd::AudioFileSourceNode*>(h); }
static uapmd::MidiClipSourceNode*   MCSN(uapmd_midi_clip_source_t h) { return reinterpret_cast<uapmd::MidiClipSourceNode*>(h); }
static uapmd::DeviceInputSourceNode* DISN(uapmd_device_input_source_t h) { return reinterpret_cast<uapmd::DeviceInputSourceNode*>(h); }
static uapmd::AudioFileReader*      AFR(uapmd_audio_file_reader_t h) { return reinterpret_cast<uapmd::AudioFileReader*>(h); }
static uapmd::UapmdProjectData*     PD(uapmd_project_data_t h)      { return reinterpret_cast<uapmd::UapmdProjectData*>(h); }
static uapmd::UapmdProjectTrackData* PTD(uapmd_project_track_data_t h) { return reinterpret_cast<uapmd::UapmdProjectTrackData*>(h); }
static uapmd::UapmdProjectPluginGraphData* PGD(uapmd_project_graph_data_t h) { return reinterpret_cast<uapmd::UapmdProjectPluginGraphData*>(h); }

static size_t copy_string(const std::string& src, char* buf, size_t buf_size) {
    size_t required = src.size() + 1;
    if (!buf || buf_size == 0)
        return required;
    size_t to_copy = (src.size() < buf_size) ? src.size() : (buf_size - 1);
    std::memcpy(buf, src.data(), to_copy);
    buf[to_copy] = '\0';
    return to_copy;
}

/* ── C ↔ C++ conversion helpers ──────────────────────────────────────────── */

static uapmd_timeline_position_t to_c(const uapmd::TimelinePosition& pos) {
    return { pos.samples, pos.legacy_beats };
}

static uapmd::TimelinePosition to_cpp(uapmd_timeline_position_t pos) {
    uapmd::TimelinePosition p;
    p.samples = pos.samples;
    p.legacy_beats = pos.legacy_beats;
    return p;
}

/* Thread-local storage for clip data conversion (strings must stay alive) */
static thread_local std::vector<uapmd::ClipData> tl_clips;
static thread_local std::vector<uapmd_clip_marker_t> tl_markers_out;
static thread_local std::vector<uapmd_audio_warp_point_t> tl_warps_out;

static void convert_clip_to_c(const uapmd::ClipData& src, uapmd_clip_data_t* out) {
    out->clip_id = src.clipId;
    out->reference_id = src.referenceId.c_str();
    out->position = to_c(src.position);
    out->duration_samples = src.durationSamples;
    out->source_node_instance_id = src.sourceNodeInstanceId;
    out->gain = src.gain;
    out->muted = src.muted;
    out->name = src.name.c_str();
    out->filepath = src.filepath.c_str();
    out->needs_file_save = src.needsFileSave;
    out->clip_type = static_cast<uapmd_clip_type_t>(src.clipType);
    out->tick_resolution = src.tickResolution;
    out->clip_tempo = src.clipTempo;
    out->nrpn_to_parameter_mapping = src.nrpnToParameterMapping;
    out->anchor_reference_id = src.anchorReferenceId.c_str();
    out->anchor_origin = static_cast<uapmd_anchor_origin_t>(src.anchorOrigin);
    out->anchor_offset = to_c(src.anchorOffset);
    out->marker_count = 0;
    out->markers = nullptr;
    out->audio_warp_count = 0;
    out->audio_warps = nullptr;
}

static uapmd::ClipData convert_clip_from_c(const uapmd_clip_data_t* src) {
    uapmd::ClipData clip;
    clip.clipId = src->clip_id;
    if (src->reference_id) clip.referenceId = src->reference_id;
    clip.position = to_cpp(src->position);
    clip.durationSamples = src->duration_samples;
    clip.sourceNodeInstanceId = src->source_node_instance_id;
    clip.gain = src->gain;
    clip.muted = src->muted;
    if (src->name) clip.name = src->name;
    if (src->filepath) clip.filepath = src->filepath;
    clip.needsFileSave = src->needs_file_save;
    clip.clipType = static_cast<uapmd::ClipType>(src->clip_type);
    clip.tickResolution = src->tick_resolution;
    clip.clipTempo = src->clip_tempo;
    clip.nrpnToParameterMapping = src->nrpn_to_parameter_mapping;
    if (src->anchor_reference_id) clip.anchorReferenceId = src->anchor_reference_id;
    clip.anchorOrigin = static_cast<uapmd::AnchorOrigin>(src->anchor_origin);
    clip.anchorOffset = to_cpp(src->anchor_offset);
    return clip;
}

static uapmd::TimeReference convert_time_ref_from_c(uapmd_time_reference_t ref) {
    uapmd::TimeReference tr;
    tr.type = static_cast<uapmd::TimeReferenceType>(ref.type);
    if (ref.reference_id) tr.referenceId = ref.reference_id;
    tr.offset = ref.offset;
    return tr;
}

static std::vector<uapmd::ClipMarker> convert_markers_from_c(const uapmd_clip_marker_t* markers, uint32_t count) {
    std::vector<uapmd::ClipMarker> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uapmd::ClipMarker m;
        if (markers[i].marker_id) m.markerId = markers[i].marker_id;
        m.clipPositionOffset = markers[i].clip_position_offset;
        m.referenceType = static_cast<uapmd::AudioWarpReferenceType>(markers[i].reference_type);
        if (markers[i].reference_clip_id) m.referenceClipId = markers[i].reference_clip_id;
        if (markers[i].reference_marker_id) m.referenceMarkerId = markers[i].reference_marker_id;
        if (markers[i].name) m.name = markers[i].name;
        result.push_back(std::move(m));
    }
    return result;
}

static std::vector<uapmd::AudioWarpPoint> convert_warps_from_c(const uapmd_audio_warp_point_t* warps, uint32_t count) {
    std::vector<uapmd::AudioWarpPoint> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uapmd::AudioWarpPoint w;
        w.clipPositionOffset = warps[i].clip_position_offset;
        w.speedRatio = warps[i].speed_ratio;
        w.referenceType = static_cast<uapmd::AudioWarpReferenceType>(warps[i].reference_type);
        if (warps[i].reference_clip_id) w.referenceClipId = warps[i].reference_clip_id;
        if (warps[i].reference_marker_id) w.referenceMarkerId = warps[i].reference_marker_id;
        result.push_back(std::move(w));
    }
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TimelinePosition helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_timeline_position_t uapmd_position_from_samples(int64_t samples, int32_t sample_rate, double tempo) {
    return to_c(uapmd::TimelinePosition::fromSamples(samples, sample_rate, tempo));
}

uapmd_timeline_position_t uapmd_position_from_beats(double beats, int32_t sample_rate, double tempo) {
    return to_c(uapmd::TimelinePosition::fromBeats(beats, sample_rate, tempo));
}

uapmd_timeline_position_t uapmd_position_from_seconds(double seconds, int32_t sample_rate, double tempo) {
    return to_c(uapmd::TimelinePosition::fromSeconds(seconds, sample_rate, tempo));
}

double uapmd_position_to_seconds(uapmd_timeline_position_t pos, int32_t sample_rate) {
    return to_cpp(pos).toSeconds(sample_rate);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ClipManager
 * ═══════════════════════════════════════════════════════════════════════════ */

int32_t uapmd_cm_add_clip(uapmd_clip_manager_t cm, const uapmd_clip_data_t* clip) {
    auto c = convert_clip_from_c(clip);
    return CM(cm)->addClip(c);
}

bool uapmd_cm_remove_clip(uapmd_clip_manager_t cm, int32_t clip_id) {
    return CM(cm)->removeClip(clip_id);
}

bool uapmd_cm_get_clip(uapmd_clip_manager_t cm, int32_t clip_id, uapmd_clip_data_t* out) {
    auto* clip = CM(cm)->getClip(clip_id);
    if (!clip) return false;
    convert_clip_to_c(*clip, out);
    return true;
}

uint32_t uapmd_cm_get_all_clips(uapmd_clip_manager_t cm, uapmd_clip_data_t* out, uint32_t out_count) {
    tl_clips = CM(cm)->getAllClips();
    uint32_t count = static_cast<uint32_t>(tl_clips.size());
    if (!out || out_count == 0)
        return count;
    uint32_t to_copy = (count < out_count) ? count : out_count;
    for (uint32_t i = 0; i < to_copy; ++i)
        convert_clip_to_c(tl_clips[i], &out[i]);
    return count;
}

size_t uapmd_cm_clip_count(uapmd_clip_manager_t cm) { return CM(cm)->clipCount(); }
void   uapmd_cm_clear_all(uapmd_clip_manager_t cm)  { CM(cm)->clearAll(); }

bool uapmd_cm_move_clip(uapmd_clip_manager_t cm, int32_t clip_id, uapmd_timeline_position_t new_position) {
    return CM(cm)->moveClip(clip_id, to_cpp(new_position));
}

bool uapmd_cm_resize_clip(uapmd_clip_manager_t cm, int32_t clip_id, int64_t new_duration) {
    return CM(cm)->resizeClip(clip_id, new_duration);
}

bool uapmd_cm_set_clip_gain(uapmd_clip_manager_t cm, int32_t clip_id, double gain) {
    return CM(cm)->setClipGain(clip_id, gain);
}

bool uapmd_cm_set_clip_muted(uapmd_clip_manager_t cm, int32_t clip_id, bool muted) {
    return CM(cm)->setClipMuted(clip_id, muted);
}

bool uapmd_cm_set_clip_name(uapmd_clip_manager_t cm, int32_t clip_id, const char* name) {
    return CM(cm)->setClipName(clip_id, name);
}

bool uapmd_cm_set_clip_filepath(uapmd_clip_manager_t cm, int32_t clip_id, const char* filepath) {
    return CM(cm)->setClipFilepath(clip_id, filepath);
}

bool uapmd_cm_set_clip_anchor(uapmd_clip_manager_t cm, int32_t clip_id,
                               uapmd_time_reference_t anchor, int32_t sample_rate) {
    return CM(cm)->setClipAnchor(clip_id, convert_time_ref_from_c(anchor), sample_rate);
}

bool uapmd_cm_set_clip_markers(uapmd_clip_manager_t cm, int32_t clip_id,
                                const uapmd_clip_marker_t* markers, uint32_t count) {
    return CM(cm)->setClipMarkers(clip_id, convert_markers_from_c(markers, count));
}

bool uapmd_cm_set_audio_warps(uapmd_clip_manager_t cm, int32_t clip_id,
                               const uapmd_audio_warp_point_t* warps, uint32_t count) {
    return CM(cm)->setAudioWarps(clip_id, convert_warps_from_c(warps, count));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SourceNode (base)
 * ═══════════════════════════════════════════════════════════════════════════ */

int32_t uapmd_sn_instance_id(uapmd_source_node_t sn) { return SN(sn)->instanceId(); }
uapmd_source_node_type_t uapmd_sn_node_type(uapmd_source_node_t sn) { return static_cast<uapmd_source_node_type_t>(SN(sn)->nodeType()); }
bool uapmd_sn_get_disabled(uapmd_source_node_t sn)      { return SN(sn)->disabled(); }
void uapmd_sn_set_disabled(uapmd_source_node_t sn, bool v) { SN(sn)->disabled(v); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

void    uapmd_asn_seek(uapmd_audio_source_node_t asn, int64_t pos) { ASN(asn)->seek(pos); }
int64_t uapmd_asn_current_position(uapmd_audio_source_node_t asn)  { return ASN(asn)->currentPosition(); }
int64_t uapmd_asn_total_length(uapmd_audio_source_node_t asn)      { return ASN(asn)->totalLength(); }
bool    uapmd_asn_is_playing(uapmd_audio_source_node_t asn)        { return ASN(asn)->isPlaying(); }
void    uapmd_asn_set_playing(uapmd_audio_source_node_t asn, bool p) { ASN(asn)->setPlaying(p); }
void    uapmd_asn_process_audio(uapmd_audio_source_node_t asn, float** buffers, uint32_t num_channels, int32_t frame_count) {
    ASN(asn)->processAudio(buffers, num_channels, frame_count);
}
uint32_t uapmd_asn_channel_count(uapmd_audio_source_node_t asn)    { return ASN(asn)->channelCount(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioFileSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

double   uapmd_afsn_sample_rate(uapmd_audio_file_source_t afsn)    { return AFSN(afsn)->sampleRate(); }
int64_t  uapmd_afsn_num_frames(uapmd_audio_file_source_t afsn)     { return AFSN(afsn)->numFrames(); }
uint32_t uapmd_afsn_audio_warp_count(uapmd_audio_file_source_t afsn) { return static_cast<uint32_t>(AFSN(afsn)->audioWarps().size()); }

bool uapmd_afsn_get_audio_warp(uapmd_audio_file_source_t afsn, uint32_t index, uapmd_audio_warp_point_t* out) {
    auto& warps = AFSN(afsn)->audioWarps();
    if (index >= warps.size()) return false;
    auto& w = warps[index];
    out->clip_position_offset = w.clipPositionOffset;
    out->speed_ratio = w.speedRatio;
    out->reference_type = static_cast<uapmd_audio_warp_reference_type_t>(w.referenceType);
    out->reference_clip_id = w.referenceClipId.c_str();
    out->reference_marker_id = w.referenceMarkerId.c_str();
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiClipSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

void    uapmd_mcsn_seek(uapmd_midi_clip_source_t mcsn, int64_t pos) { MCSN(mcsn)->seek(pos); }
int64_t uapmd_mcsn_current_position(uapmd_midi_clip_source_t mcsn)  { return MCSN(mcsn)->currentPosition(); }
int64_t uapmd_mcsn_total_length(uapmd_midi_clip_source_t mcsn)      { return MCSN(mcsn)->totalLength(); }
bool    uapmd_mcsn_is_playing(uapmd_midi_clip_source_t mcsn)        { return MCSN(mcsn)->isPlaying(); }
void    uapmd_mcsn_set_playing(uapmd_midi_clip_source_t mcsn, bool p) { MCSN(mcsn)->setPlaying(p); }
uint32_t uapmd_mcsn_tick_resolution(uapmd_midi_clip_source_t mcsn)   { return MCSN(mcsn)->tickResolution(); }
double   uapmd_mcsn_clip_tempo(uapmd_midi_clip_source_t mcsn)        { return MCSN(mcsn)->clipTempo(); }
uint32_t uapmd_mcsn_ump_event_count(uapmd_midi_clip_source_t mcsn)   { return static_cast<uint32_t>(MCSN(mcsn)->umpEvents().size()); }
const uapmd_ump_t* uapmd_mcsn_ump_events(uapmd_midi_clip_source_t mcsn) { return MCSN(mcsn)->umpEvents().data(); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  DeviceInputSourceNode
 * ═══════════════════════════════════════════════════════════════════════════ */

void uapmd_disn_set_device_input_buffers(uapmd_device_input_source_t disn, float** device_buffers, uint32_t device_channel_count) {
    DISN(disn)->setDeviceInputBuffers(device_buffers, device_channel_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioFileReader / AudioFileFactory
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "c-api-internal.h"

uapmd_audio_file_reader_t uapmd_audio_file_reader_create(const char* filepath) {
    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
    if (!reader)
        return nullptr;
    auto raw = reader.get();
    std::lock_guard lock(s_reader_mutex);
    s_owned_readers[raw] = std::move(reader);
    return reinterpret_cast<uapmd_audio_file_reader_t>(raw);
}

void uapmd_audio_file_reader_destroy(uapmd_audio_file_reader_t reader) {
    if (!reader) return;
    std::lock_guard lock(s_reader_mutex);
    s_owned_readers.erase(AFR(reader));
}

bool uapmd_audio_file_reader_get_properties(uapmd_audio_file_reader_t reader, uapmd_audio_file_properties_t* out) {
    if (!reader) return false;
    auto props = AFR(reader)->getProperties();
    out->num_frames = props.numFrames;
    out->num_channels = props.numChannels;
    out->sample_rate = props.sampleRate;
    return true;
}

void uapmd_audio_file_reader_read_frames(uapmd_audio_file_reader_t reader,
                                          uint64_t start_frame,
                                          uint64_t frames_to_read,
                                          float* const* dest,
                                          uint32_t num_channels) {
    if (reader)
        AFR(reader)->readFrames(start_frame, frames_to_read, dest, num_channels);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SmfConverter
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Internal struct that owns the data behind an smf_convert_result */
struct SmfConvertResultStorage {
    uapmd_smf_convert_result_t result{};
    std::string error;
    std::vector<uapmd_ump_t> ump_events;
    std::vector<uint64_t> tick_stamps;
    std::vector<uapmd_midi_tempo_change_t> tempo_changes;
    std::vector<uapmd_midi_time_sig_change_t> time_sig_changes;
};

static uapmd_smf_convert_result_t* make_smf_result(const uapmd::SmfConverter::ConvertResult& src) {
    auto* storage = new SmfConvertResultStorage;
    storage->error = src.error;
    storage->ump_events = src.umpEvents;
    storage->tick_stamps = src.umpEventTicksStamps;

    storage->tempo_changes.resize(src.tempoChanges.size());
    for (size_t i = 0; i < src.tempoChanges.size(); ++i) {
        storage->tempo_changes[i].tick_position = src.tempoChanges[i].tickPosition;
        storage->tempo_changes[i].bpm = src.tempoChanges[i].bpm;
    }

    storage->time_sig_changes.resize(src.timeSignatureChanges.size());
    for (size_t i = 0; i < src.timeSignatureChanges.size(); ++i) {
        auto& s = src.timeSignatureChanges[i];
        storage->time_sig_changes[i] = { s.tickPosition, s.numerator, s.denominator, s.clocksPerClick, s.thirtySecondsPerQuarter };
    }

    auto& r = storage->result;
    r.success = src.success;
    r.error = storage->error.empty() ? nullptr : storage->error.c_str();
    r.ump_events = storage->ump_events.data();
    r.ump_event_count = static_cast<uint32_t>(storage->ump_events.size());
    r.ump_event_tick_stamps = storage->tick_stamps.data();
    r.tick_stamp_count = static_cast<uint32_t>(storage->tick_stamps.size());
    r.tempo_changes = storage->tempo_changes.data();
    r.tempo_change_count = static_cast<uint32_t>(storage->tempo_changes.size());
    r.time_sig_changes = storage->time_sig_changes.data();
    r.time_sig_change_count = static_cast<uint32_t>(storage->time_sig_changes.size());
    r.tick_resolution = src.tickResolution;
    r.detected_tempo = src.detectedTempo;
    return &storage->result;
}

uapmd_smf_convert_result_t* uapmd_smf_convert_to_ump(const char* smf_file_path) {
    auto result = uapmd::SmfConverter::convertToUmp(smf_file_path);
    return make_smf_result(result);
}

uapmd_smf_convert_result_t* uapmd_smf_convert_track_to_ump(const char* smf_file_path, uint32_t track_index) {
    auto result = uapmd::SmfConverter::convertTrackToUmp(smf_file_path, track_index);
    return make_smf_result(result);
}

void uapmd_smf_convert_result_free(uapmd_smf_convert_result_t* result) {
    if (!result) return;
    /* The result pointer is &storage->result, which is at offset 0 of SmfConvertResultStorage */
    auto* storage = reinterpret_cast<SmfConvertResultStorage*>(result);
    delete storage;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MidiClipReader
 * ═══════════════════════════════════════════════════════════════════════════ */

static uapmd_smf_convert_result_t* make_clip_result(const uapmd::MidiClipReader::ClipInfo& info) {
    uapmd::SmfConverter::ConvertResult compat;
    compat.success = info.success;
    compat.error = info.error;
    compat.umpEvents = info.ump_data;
    compat.umpEventTicksStamps = info.ump_tick_timestamps;
    compat.tempoChanges = info.tempo_changes;
    compat.timeSignatureChanges = info.time_signature_changes;
    compat.tickResolution = info.tick_resolution;
    compat.detectedTempo = info.tempo;
    return make_smf_result(compat);
}

uapmd_smf_convert_result_t* uapmd_midi_clip_read_any_format(const char* file_path) {
    auto info = uapmd::MidiClipReader::readAnyFormat(file_path);
    return make_clip_result(info);
}

bool uapmd_midi_clip_is_valid_smf2(const char* file_path) {
    return uapmd::MidiClipReader::isValidSmf2Clip(file_path);
}

bool uapmd_midi_clip_is_valid_smf(const char* file_path) {
    return uapmd::MidiClipReader::isValidSmfFile(file_path);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ProjectArchive
 * ═══════════════════════════════════════════════════════════════════════════ */

bool uapmd_project_archive_is_archive(const char* path) {
    return uapmd::ProjectArchive::isArchive(path);
}

struct ProjectArchiveExtractStorage {
    uapmd_project_archive_extract_result_t result{};
    std::string error;
    std::string project_file;
};

uapmd_project_archive_extract_result_t* uapmd_project_archive_extract(const char* archive_path, const char* destination_dir) {
    auto r = uapmd::ProjectArchive::extractArchive(archive_path, destination_dir);
    auto* storage = new ProjectArchiveExtractStorage;
    storage->error = r.error;
    storage->project_file = r.projectFile.string();
    storage->result.success = r.success;
    storage->result.error = storage->error.empty() ? nullptr : storage->error.c_str();
    storage->result.project_file = storage->project_file.empty() ? nullptr : storage->project_file.c_str();
    return &storage->result;
}

void uapmd_project_archive_extract_result_free(uapmd_project_archive_extract_result_t* result) {
    if (!result) return;
    delete reinterpret_cast<ProjectArchiveExtractStorage*>(result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdProjectData
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::mutex s_project_mutex;
static std::unordered_map<uapmd::UapmdProjectData*, std::unique_ptr<uapmd::UapmdProjectData>> s_owned_projects;

uapmd_project_data_t uapmd_project_data_create() {
    auto data = uapmd::UapmdProjectData::create();
    auto raw = data.get();
    std::lock_guard lock(s_project_mutex);
    s_owned_projects[raw] = std::move(data);
    return reinterpret_cast<uapmd_project_data_t>(raw);
}

void uapmd_project_data_destroy(uapmd_project_data_t data) {
    if (!data) return;
    std::lock_guard lock(s_project_mutex);
    s_owned_projects.erase(PD(data));
}

uint32_t uapmd_project_data_track_count(uapmd_project_data_t data) {
    return static_cast<uint32_t>(PD(data)->tracks().size());
}

uapmd_project_track_data_t uapmd_project_data_get_track(uapmd_project_data_t data, uint32_t index) {
    auto& tracks = PD(data)->tracks();
    if (index >= tracks.size()) return nullptr;
    return reinterpret_cast<uapmd_project_track_data_t>(tracks[index]);
}

uapmd_project_track_data_t uapmd_project_data_master_track(uapmd_project_data_t data) {
    return reinterpret_cast<uapmd_project_track_data_t>(PD(data)->masterTrack());
}

bool uapmd_project_data_remove_track(uapmd_project_data_t data, uint32_t index) {
    return PD(data)->removeTrack(index);
}

bool uapmd_project_data_write(uapmd_project_data_t data, const char* file_path) {
    return uapmd::UapmdProjectDataWriter::write(PD(data), file_path);
}

uapmd_project_data_t uapmd_project_data_read(const char* file_path) {
    auto data = uapmd::UapmdProjectDataReader::read(file_path);
    if (!data)
        return nullptr;
    auto raw = data.get();
    std::lock_guard lock(s_project_mutex);
    s_owned_projects[raw] = std::move(data);
    return reinterpret_cast<uapmd_project_data_t>(raw);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UapmdProjectPluginGraphData
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::mutex s_graph_data_mutex;
static std::unordered_map<uapmd::UapmdProjectPluginGraphData*, std::unique_ptr<uapmd::UapmdProjectPluginGraphData>> s_owned_graph_data;

uapmd_project_graph_data_t uapmd_project_graph_data_create() {
    auto data = uapmd::UapmdProjectPluginGraphData::create();
    auto raw = data.get();
    std::lock_guard lock(s_graph_data_mutex);
    s_owned_graph_data[raw] = std::move(data);
    return reinterpret_cast<uapmd_project_graph_data_t>(raw);
}

size_t uapmd_project_graph_get_graph_type(uapmd_project_graph_data_t graph, char* buf, size_t buf_size) {
    return copy_string(PGD(graph)->graphType(), buf, buf_size);
}

void uapmd_project_graph_set_graph_type(uapmd_project_graph_data_t graph, const char* type) {
    PGD(graph)->graphType(type ? type : "");
}

size_t uapmd_project_graph_get_external_file(uapmd_project_graph_data_t graph, char* buf, size_t buf_size) {
    return copy_string(PGD(graph)->externalFile().string(), buf, buf_size);
}

void uapmd_project_graph_set_external_file(uapmd_project_graph_data_t graph, const char* path) {
    PGD(graph)->externalFile(path ? path : "");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TimelineTrack
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_clip_manager_t uapmd_tt_clip_manager(uapmd_timeline_track_t tt) {
    return reinterpret_cast<uapmd_clip_manager_t>(&TT(tt)->clipManager());
}

uint32_t uapmd_tt_channel_count(uapmd_timeline_track_t tt) { return TT(tt)->channelCount(); }
double   uapmd_tt_sample_rate(uapmd_timeline_track_t tt)   { return TT(tt)->sampleRate(); }

size_t uapmd_tt_reference_id(uapmd_timeline_track_t tt, char* buf, size_t buf_size) {
    return copy_string(TT(tt)->referenceId(), buf, buf_size);
}

bool uapmd_tt_has_device_input_source(uapmd_timeline_track_t tt) { return TT(tt)->hasDeviceInputSource(); }
bool uapmd_tt_remove_clip(uapmd_timeline_track_t tt, int32_t clip_id) { return TT(tt)->removeClip(clip_id); }
