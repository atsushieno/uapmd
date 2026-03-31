#include "McpServer.hpp"
#include "../AppModel.hpp"
#include "../UapmdJSRuntime.hpp"

#ifdef UAPMD_MCP_HAS_HTTP_SERVER
#include <httplib.h>
#endif
#ifndef __EMSCRIPTEN__
#include <ixwebsocket/IXWebSocket.h>
#else
#include <emscripten.h>
#endif
#include <choc/javascript/choc_javascript.h>
#include <choc/text/choc_JSON.h>
#include <AppJsLib.h>
#include <ResEmbed/ResEmbed.h>

#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>

namespace uapmd {

// ─────────────────────────────────────────────────
//  JSON-RPC 2.0 helpers
// ─────────────────────────────────────────────────

static std::string jsonRpcResult(const choc::value::Value& id, const choc::value::Value& result)
{
    auto resp = choc::value::createObject ("");
    resp.setMember ("jsonrpc", std::string ("2.0"));
    resp.setMember ("id", id);
    resp.setMember ("result", result);
    return choc::json::toString (resp);
}

static std::string jsonRpcError(const choc::value::Value& id, int code, const std::string& message)
{
    auto resp = choc::value::createObject ("");
    resp.setMember ("jsonrpc", std::string ("2.0"));
    resp.setMember ("id", id);
    auto err = choc::value::createObject ("");
    err.setMember ("code", code);
    err.setMember ("message", message);
    resp.setMember ("error", err);
    return choc::json::toString (resp);
}

static choc::value::Value textContent(const std::string& text)
{
    auto arr = choc::value::createEmptyArray();
    auto item = choc::value::createObject ("");
    item.setMember ("type", std::string ("text"));
    item.setMember ("text", text);
    arr.addArrayElement (item);
    return arr;
}

static std::string getStringArg(const choc::value::Value& args, const char* key, const std::string& fallback = "")
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<std::string>();
    return fallback;
}

static int32_t getIntArg(const choc::value::Value& args, const char* key, int32_t fallback = -1)
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<int32_t>();
    return fallback;
}

static int64_t getInt64Arg(const choc::value::Value& args, const char* key, int64_t fallback = 0)
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<int64_t>();
    return fallback;
}

static double getDoubleArg(const choc::value::Value& args, const char* key, double fallback = 0.0)
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<double>();
    return fallback;
}

namespace {
constexpr std::string_view kMasterMarkerReferenceId = "master_track";

std::string timeReferenceTypeToString(uapmd::TimeReferenceType type)
{
    switch (type) {
        case uapmd::TimeReferenceType::ContainerStart: return "containerStart";
        case uapmd::TimeReferenceType::ContainerEnd: return "containerEnd";
        case uapmd::TimeReferenceType::Point: return "point";
    }
    return "containerStart";
}

bool parseTimeReferenceType(std::string_view text, uapmd::TimeReferenceType& type)
{
    if (text == "containerStart") { type = uapmd::TimeReferenceType::ContainerStart; return true; }
    if (text == "containerEnd") { type = uapmd::TimeReferenceType::ContainerEnd; return true; }
    if (text == "point") { type = uapmd::TimeReferenceType::Point; return true; }
    return false;
}

choc::value::Value serializeTimeReference(const uapmd::TimeReference& reference)
{
    auto obj = choc::value::createObject ("");
    obj.setMember ("type", timeReferenceTypeToString(reference.type));
    obj.setMember ("referenceId", reference.referenceId);
    obj.setMember ("offset", reference.offset);
    return obj;
}

choc::value::Value serializeMarker(const uapmd::ClipMarker& marker, std::string_view ownerReferenceId)
{
    auto obj = choc::value::createObject ("");
    obj.setMember ("markerId", marker.markerId);
    obj.setMember ("name", marker.name);
    obj.setMember ("timeReference", serializeTimeReference(marker.timeReference(ownerReferenceId, kMasterMarkerReferenceId)));
    return obj;
}

choc::value::Value serializeWarp(const uapmd::AudioWarpPoint& warp, std::string_view ownerReferenceId)
{
    auto obj = choc::value::createObject ("");
    obj.setMember ("speedRatio", warp.speedRatio);
    obj.setMember ("timeReference", serializeTimeReference(warp.timeReference(ownerReferenceId, kMasterMarkerReferenceId)));
    return obj;
}

bool parseTimeReferenceValue(const choc::value::ValueView& value, uapmd::TimeReference& reference, std::string& error)
{
    if (!value.isObject()) {
        error = "timeReference must be an object";
        return false;
    }
    const auto typeText = value.hasObjectMember("type") ? std::string(value["type"].getString()) : std::string{};
    if (!parseTimeReferenceType(typeText, reference.type)) {
        error = "timeReference.type is invalid";
        return false;
    }
    reference.referenceId = value.hasObjectMember("referenceId") ? std::string(value["referenceId"].getString()) : std::string{};
    reference.offset = value.hasObjectMember("offset") ? value["offset"].getWithDefault<double>(0.0) : 0.0;
    return true;
}

bool parseMarkersArg(const choc::value::Value& args, const char* key, std::string_view ownerReferenceId,
                     std::vector<uapmd::ClipMarker>& markers, std::string& error)
{
    markers.clear();
    if (!args.isObject() || !args.hasObjectMember(key))
        return true;
    auto array = args[key];
    if (!array.isArray()) {
        error = "markers must be an array";
        return false;
    }
    for (const auto& item : array) {
        if (!item.isObject()) {
            error = "marker entry must be an object";
            return false;
        }
        uapmd::ClipMarker marker;
        if (item.hasObjectMember("markerId"))
            marker.markerId = std::string(item["markerId"].getString());
        if (item.hasObjectMember("name"))
            marker.name = std::string(item["name"].getString());
        uapmd::TimeReference reference;
        if (!item.hasObjectMember("timeReference") || !parseTimeReferenceValue(item["timeReference"], reference, error))
            return false;
        marker.setTimeReference(reference, ownerReferenceId, kMasterMarkerReferenceId);
        markers.push_back(std::move(marker));
    }
    return true;
}

bool parseWarpsArg(const choc::value::Value& args, const char* key, std::string_view ownerReferenceId,
                   std::vector<uapmd::AudioWarpPoint>& warps, std::string& error)
{
    warps.clear();
    if (!args.isObject() || !args.hasObjectMember(key))
        return true;
    auto array = args[key];
    if (!array.isArray()) {
        error = "audioWarps must be an array";
        return false;
    }
    for (const auto& item : array) {
        if (!item.isObject()) {
            error = "audioWarp entry must be an object";
            return false;
        }
        uapmd::AudioWarpPoint warp;
        warp.speedRatio = item.hasObjectMember("speedRatio") ? item["speedRatio"].getWithDefault<double>(1.0) : 1.0;
        uapmd::TimeReference reference;
        if (!item.hasObjectMember("timeReference") || !parseTimeReferenceValue(item["timeReference"], reference, error))
            return false;
        warp.setTimeReference(reference, ownerReferenceId, kMasterMarkerReferenceId);
        warps.push_back(std::move(warp));
    }
    return true;
}
}

// ─────────────────────────────────────────────────
//  Tool definitions
// ─────────────────────────────────────────────────

// Use the R"j(...)j" delimiter to avoid premature termination on )"  inside JSON strings.
static choc::value::Value buildToolDefinitions()
{
    auto tools = choc::value::createEmptyArray();

    struct ToolDef { const char* name; const char* description; const char* schemaJson; };

    static const ToolDef defs[] = {
        {
            "list_plugins",
            "List available plugins. Optionally filter by format (VST3, AU, LV2, CLAP).",
            R"j({"type":"object","properties":{"format":{"type":"string","description":"Format filter: VST3, AU, LV2, CLAP"}}})j"
        },
        {
            "list_tracks",
            "List all sequencer tracks with their plugin instances.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "create_track",
            "Create a new empty sequencer track. Returns the new trackIndex.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "add_plugin_to_track",
            "Add a plugin instance to a track. Returns the instanceId.",
            R"j({"type":"object","required":["trackIndex","pluginId","format"],"properties":{"trackIndex":{"type":"integer"},"pluginId":{"type":"string","description":"Plugin ID from list_plugins"},"format":{"type":"string","description":"VST3, AU, LV2, or CLAP"}}})j"
        },
        {
            "get_timeline_state",
            "Get tempo, time signature, playback position, and loop settings.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "set_tempo",
            "Set the timeline tempo in BPM.",
            R"j({"type":"object","required":["bpm"],"properties":{"bpm":{"type":"number"}}})j"
        },
        {
            "list_clips",
            "List all clips on a given track.",
            R"j({"type":"object","required":["trackIndex"],"properties":{"trackIndex":{"type":"integer"}}})j"
        },
        {
            "get_clip_audio_events",
            "Get markers and audio warps for a clip.",
            R"j({"type":"object","required":["trackIndex","clipId"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"}}})j"
        },
        {
            "set_clip_audio_events",
            "Replace markers and audio warps for an audio clip. Uses timeReference objects with type, referenceId, and offset.",
            R"j({"type":"object","required":["trackIndex","clipId"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"},"markers":{"type":"array"},"audioWarps":{"type":"array"}}})j"
        },
        {
            "get_master_markers",
            "Get master-track markers.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "set_master_markers",
            "Replace master-track markers. Uses timeReference objects with type, referenceId, and offset.",
            R"j({"type":"object","required":["markers"],"properties":{"markers":{"type":"array"}}})j"
        },
        {
            "add_midi_clip",
            "Import a Standard MIDI File (.mid) as a clip on a track.",
            R"j({"type":"object","required":["trackIndex","filepath"],"properties":{"trackIndex":{"type":"integer"},"positionSamples":{"type":"integer","description":"Start in samples (default 0)"},"filepath":{"type":"string","description":"Absolute path to .mid file"}}})j"
        },
        {
            "remove_clip",
            "Remove a clip from a track by clipId.",
            R"j({"type":"object","required":["trackIndex","clipId"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"}}})j"
        },
        {
            "get_clip_ump_events",
            "Get the raw UMP event stream of a MIDI clip. "
            "Returns tickResolution, bpm, and events[]{eventIndex, tick, words[]}. "
            "words is 1 element for MIDI1 messages, 2 for MIDI2.",
            R"j({"type":"object","required":["trackIndex","clipId"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"}}})j"
        },
        {
            "add_ump_event",
            "Insert a single UMP event into a MIDI clip at a given tick position. "
            "words must contain exactly the right number of uint32 values for the message type "
            "(1 for MIDI1, 2 for MIDI2). Use get_clip_ump_events to inspect existing events.",
            R"j({"type":"object","required":["trackIndex","clipId","tick","words"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"},"tick":{"type":"integer","description":"Tick position within the clip (based on tickResolution)"},"words":{"type":"array","items":{"type":"integer"},"description":"1 or 2 uint32 UMP words as integers"}}})j"
        },
        {
            "remove_ump_event",
            "Remove a UMP event from a MIDI clip by its eventIndex (as returned by get_clip_ump_events). "
            "Removes all words belonging to that logical event (1 for MIDI1, 2 for MIDI2).",
            R"j({"type":"object","required":["trackIndex","clipId","eventIndex"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"},"eventIndex":{"type":"integer","description":"Zero-based logical event index from get_clip_ump_events"}}})j"
        },
        {
            "create_empty_midi_clip",
            "Create a new empty MIDI clip on a track. Returns clipId. "
            "Use add_ump_event to populate it with events.",
            R"j({"type":"object","required":["trackIndex"],"properties":{"trackIndex":{"type":"integer"},"positionSamples":{"type":"integer","description":"Start position in samples (default 0)"},"tickResolution":{"type":"integer","description":"Ticks per quarter note (default 480)"},"bpm":{"type":"number","description":"Clip tempo in BPM (default 120)"}}})j"
        },
        {
            "play",
            "Start timeline playback.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "stop",
            "Stop timeline playback.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "run_script",
            "Execute JavaScript using the uapmd scripting API. "
            "All __remidy_* native functions and the uapmd.* global object are available. "
            "Returns the string representation of the last evaluated expression.",
            R"j({"type":"object","required":["code"],"properties":{"code":{"type":"string","description":"JavaScript code to execute"}}})j"
        },
        {
            "get_presets",
            "Get the list of available presets for a plugin instance.",
            R"j({"type":"object","required":["instanceId"],"properties":{"instanceId":{"type":"integer"}}})j"
        },
        {
            "load_preset",
            "Load a specific preset for a plugin instance.",
            R"j({"type":"object","required":["instanceId","presetIndex"],"properties":{"instanceId":{"type":"integer"},"presetIndex":{"type":"integer"}}})j"
        },
    };

    for (const auto& def : defs)
    {
        auto t = choc::value::createObject ("");
        t.setMember ("name", std::string (def.name));
        t.setMember ("description", std::string (def.description));
        t.setMember ("inputSchema", choc::json::parse (def.schemaJson));
        tools.addArrayElement (t);
    }

    return tools;
}

// ─────────────────────────────────────────────────
//  Tool handlers (all run on main thread)
// ─────────────────────────────────────────────────

static choc::value::Value toolListPlugins(const choc::value::Value& args)
{
    auto formatFilter = getStringArg (args, "format");
    auto& seq = AppModel::instance().sequencer();
    auto plugins = seq.engine()->pluginHost()->pluginCatalogEntries();

    auto arr = choc::value::createEmptyArray();
    for (auto p : plugins)           // non-const copy — catalog methods are not const
    {
        if (!formatFilter.empty() && p.format() != formatFilter)
            continue;
        auto obj = choc::value::createObject ("");
        obj.setMember ("pluginId", p.pluginId());
        obj.setMember ("displayName", p.displayName());
        obj.setMember ("format", p.format());
        obj.setMember ("vendorName", p.vendorName());
        arr.addArrayElement (obj);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("plugins", arr);
    return result;
}

static choc::value::Value toolListTracks(const choc::value::Value&)
{
    auto& seq = AppModel::instance().sequencer();
    auto& engine = *seq.engine();
    const auto& tracks = engine.tracks();

    auto arr = choc::value::createEmptyArray();
    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i)
    {
        auto t = choc::value::createObject ("");
        t.setMember ("trackIndex", i);
        auto instances = choc::value::createEmptyArray();
        for (const auto& [instanceId, node] : tracks[static_cast<size_t>(i)]->graph().plugins())
        {
            if (!node) continue;
            auto inst = choc::value::createObject ("");
            inst.setMember ("instanceId", instanceId);
            auto* pluginInst = engine.getPluginInstance (instanceId);
            inst.setMember ("pluginName", pluginInst ? pluginInst->displayName() : std::string (""));
            instances.addArrayElement (inst);
        }
        t.setMember ("instances", instances);
        arr.addArrayElement (t);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("tracks", arr);
    return result;
}

static choc::value::Value toolCreateTrack(const choc::value::Value&)
{
    auto trackIndex = AppModel::instance().addTrack();
    auto result = choc::value::createObject ("");
    result.setMember ("trackIndex", trackIndex);
    return result;
}

static choc::value::Value toolAddPluginToTrack(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    auto pluginId   = getStringArg (args, "pluginId");
    auto format     = getStringArg (args, "format");

    if (trackIndex < 0 || pluginId.empty() || format.empty())
        throw std::invalid_argument ("trackIndex, pluginId and format are required");

    auto& model = AppModel::instance();
    AppModel::PluginInstanceConfig config{};

    std::atomic<int32_t> resultId{-1};
    std::atomic<bool> completed{false};
    std::string error;

    auto callback = [&](const AppModel::PluginInstanceResult& r) {
        if (r.instanceId < 0 || !r.error.empty())
            error = r.error.empty() ? "Failed to create instance" : r.error;
        else
            resultId = r.instanceId;
        completed = true;
    };

    model.instanceCreated.push_back (callback);
    model.createPluginInstanceAsync (format, pluginId, trackIndex, config);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (!completed && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    model.instanceCreated.pop_back();

    if (!completed)
        throw std::runtime_error ("Timeout creating plugin instance");
    if (!error.empty())
        throw std::runtime_error (error);

    auto result = choc::value::createObject ("");
    result.setMember ("instanceId", resultId.load());
    return result;
}

static choc::value::Value toolGetTimelineState(const choc::value::Value&)
{
    const auto& state = AppModel::instance().timeline();
    auto result = choc::value::createObject ("");
    result.setMember ("tempo", state.tempo);
    result.setMember ("timeSignatureNumerator", state.timeSignatureNumerator);
    result.setMember ("timeSignatureDenominator", state.timeSignatureDenominator);
    result.setMember ("isPlaying", state.isPlaying);
    result.setMember ("playheadSamples", choc::value::createInt64 (state.playheadPosition.samples));
    result.setMember ("loopEnabled", state.loopEnabled);
    result.setMember ("loopStartSamples", choc::value::createInt64 (state.loopStart.samples));
    result.setMember ("loopEndSamples", choc::value::createInt64 (state.loopEnd.samples));
    return result;
}

static choc::value::Value toolSetTempo(const choc::value::Value& args)
{
    double bpm = 120.0;
    if (args.isObject() && args.hasObjectMember ("bpm"))
        bpm = args["bpm"].get<double>();
    if (bpm <= 0.0)
        throw std::invalid_argument ("bpm must be positive");
    AppModel::instance().timeline().tempo = bpm;
    auto result = choc::value::createObject ("");
    result.setMember ("tempo", bpm);
    return result;
}

static choc::value::Value toolListClips(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    if (trackIndex < 0)
        throw std::invalid_argument ("trackIndex is required");

    auto tracks = AppModel::instance().getTimelineTracks();
    if (trackIndex >= static_cast<int32_t>(tracks.size()))
        throw std::out_of_range ("trackIndex out of range");

    const auto clips = tracks[static_cast<size_t>(trackIndex)]->clipManager().getAllClips();
    auto arr = choc::value::createEmptyArray();
    for (const auto& clip : clips)
    {
        auto obj = choc::value::createObject ("");
        obj.setMember ("clipId", clip.clipId);
        obj.setMember ("positionSamples", choc::value::createInt64 (clip.position.samples));
        obj.setMember ("durationSamples", choc::value::createInt64 (clip.durationSamples));
        obj.setMember ("name", clip.name);
        obj.setMember ("filepath", clip.filepath);
        obj.setMember ("clipType", clip.clipType == ClipType::Midi ? std::string ("midi") : std::string ("audio"));
        obj.setMember ("gain", clip.gain);
        obj.setMember ("muted", clip.muted);
        obj.setMember ("tempo", clip.clipTempo);
        obj.setMember ("tickResolution", static_cast<int32_t> (clip.tickResolution));
        arr.addArrayElement (obj);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("clips", arr);
    return result;
}

static choc::value::Value toolGetClipAudioEvents(const choc::value::Value& args)
{
    auto trackIndex = getIntArg(args, "trackIndex", -1);
    auto clipId = getIntArg(args, "clipId", -1);
    std::vector<uapmd::ClipMarker> markers;
    std::vector<uapmd::AudioWarpPoint> warps;
    std::string error;

    if (!AppModel::instance().getClipAudioEvents(trackIndex, clipId, markers, warps, error))
        throw std::invalid_argument(error);

    std::string ownerReferenceId = std::string(kMasterMarkerReferenceId);
    if (trackIndex != uapmd::kMasterTrackIndex) {
        auto tracks = AppModel::instance().getTimelineTracks();
        if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()) && tracks[trackIndex]) {
            if (auto* clip = tracks[trackIndex]->clipManager().getClip(clipId))
                ownerReferenceId = clip->referenceId;
        }
    }

    auto result = choc::value::createObject ("");
    auto markerArray = choc::value::createEmptyArray();
    for (const auto& marker : markers)
        markerArray.addArrayElement(serializeMarker(marker, ownerReferenceId));
    auto warpArray = choc::value::createEmptyArray();
    for (const auto& warp : warps)
        warpArray.addArrayElement(serializeWarp(warp, ownerReferenceId));
    result.setMember("markers", markerArray);
    result.setMember("audioWarps", warpArray);
    return result;
}

static choc::value::Value toolSetClipAudioEvents(const choc::value::Value& args)
{
    auto trackIndex = getIntArg(args, "trackIndex", -1);
    auto clipId = getIntArg(args, "clipId", -1);
    auto tracks = AppModel::instance().getTimelineTracks();
    std::string ownerReferenceId = std::string(kMasterMarkerReferenceId);
    if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()) && tracks[trackIndex]) {
        if (auto* clip = tracks[trackIndex]->clipManager().getClip(clipId))
            ownerReferenceId = clip->referenceId;
    }

    std::vector<uapmd::ClipMarker> markers;
    std::vector<uapmd::AudioWarpPoint> warps;
    std::string error;
    if (!parseMarkersArg(args, "markers", ownerReferenceId, markers, error) ||
        !parseWarpsArg(args, "audioWarps", ownerReferenceId, warps, error) ||
        !AppModel::instance().setClipAudioEvents(trackIndex, clipId, std::move(markers), std::move(warps), error)) {
        throw std::invalid_argument(error);
    }

    auto result = choc::value::createObject ("");
    result.setMember("success", true);
    return result;
}

static choc::value::Value toolGetMasterMarkers(const choc::value::Value&)
{
    auto result = choc::value::createObject ("");
    auto markerArray = choc::value::createEmptyArray();
    for (const auto& marker : AppModel::instance().masterTrackMarkers())
        markerArray.addArrayElement(serializeMarker(marker, kMasterMarkerReferenceId));
    result.setMember("markers", markerArray);
    return result;
}

static choc::value::Value toolSetMasterMarkers(const choc::value::Value& args)
{
    std::vector<uapmd::ClipMarker> markers;
    std::string error;
    if (!parseMarkersArg(args, "markers", kMasterMarkerReferenceId, markers, error) ||
        !AppModel::instance().setMasterTrackMarkersWithValidation(std::move(markers), error)) {
        throw std::invalid_argument(error);
    }

    auto result = choc::value::createObject ("");
    result.setMember("success", true);
    return result;
}

static choc::value::Value toolAddMidiClip(const choc::value::Value& args)
{
    auto trackIndex      = getIntArg (args, "trackIndex");
    auto filepath        = getStringArg (args, "filepath");
    auto positionSamples = getInt64Arg (args, "positionSamples", 0);

    if (trackIndex < 0 || filepath.empty())
        throw std::invalid_argument ("trackIndex and filepath are required");

    TimelinePosition pos{};
    pos.samples = positionSamples;
    auto r = AppModel::instance().addMidiClipToTrack (trackIndex, pos, filepath);

    if (!r.success)
        throw std::runtime_error (r.error.empty() ? "Failed to add MIDI clip" : r.error);

    auto result = choc::value::createObject ("");
    result.setMember ("clipId", r.clipId);
    return result;
}

static choc::value::Value toolRemoveClip(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    auto clipId     = getIntArg (args, "clipId");

    if (trackIndex < 0 || clipId < 0)
        throw std::invalid_argument ("trackIndex and clipId are required");

    bool ok = AppModel::instance().removeClipFromTrack (trackIndex, clipId);
    auto result = choc::value::createObject ("");
    result.setMember ("success", ok);
    return result;
}

static choc::value::Value toolGetClipUmpEvents(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    auto clipId     = getIntArg (args, "clipId");
    if (trackIndex < 0 || clipId < 0)
        throw std::invalid_argument ("trackIndex and clipId are required");
    return AppModel::instance().getMidiClipUmpEvents (trackIndex, clipId);
}

static choc::value::Value toolAddUmpEvent(const choc::value::Value& args)
{
    auto trackIndex = getIntArg  (args, "trackIndex");
    auto clipId     = getIntArg  (args, "clipId");
    auto tick       = static_cast<uint64_t>(getInt64Arg (args, "tick", 0));
    if (trackIndex < 0 || clipId < 0)
        throw std::invalid_argument ("trackIndex and clipId are required");

    std::vector<uint32_t> words;
    if (args.isObject() && args.hasObjectMember ("words")) {
        auto wordsVal = args["words"];
        if (wordsVal.isArray())
            for (uint32_t i = 0; i < wordsVal.size(); ++i)
                words.push_back (static_cast<uint32_t>(wordsVal[i].get<int64_t>()));
    }
    if (words.empty())
        throw std::invalid_argument ("words is required and must be non-empty");

    std::string error;
    if (!AppModel::instance().addUmpEventToClip (trackIndex, clipId, tick, std::move(words), error))
        throw std::runtime_error (error.empty() ? "Failed to add UMP event" : error);
    return choc::value::createObject ("");
}

static choc::value::Value toolRemoveUmpEvent(const choc::value::Value& args)
{
    auto trackIndex  = getIntArg (args, "trackIndex");
    auto clipId      = getIntArg (args, "clipId");
    auto eventIndex  = getIntArg (args, "eventIndex");
    if (trackIndex < 0 || clipId < 0 || eventIndex < 0)
        throw std::invalid_argument ("trackIndex, clipId and eventIndex are required");

    std::string error;
    if (!AppModel::instance().removeUmpEventFromClip (trackIndex, clipId, eventIndex, error))
        throw std::runtime_error (error.empty() ? "Failed to remove UMP event" : error);
    return choc::value::createObject ("");
}

static choc::value::Value toolCreateEmptyMidiClip(const choc::value::Value& args)
{
    auto trackIndex      = getIntArg   (args, "trackIndex");
    if (trackIndex < 0)
        throw std::invalid_argument ("trackIndex is required");
    auto positionSamples = getInt64Arg (args, "positionSamples", 0);
    auto tickResolution  = static_cast<uint32_t>(std::max (1, getIntArg (args, "tickResolution", 480)));
    auto bpm             = getDoubleArg(args, "bpm", 120.0);

    auto r = AppModel::instance().createEmptyMidiClip (trackIndex, positionSamples, tickResolution, bpm);
    if (!r.success)
        throw std::runtime_error (r.error.empty() ? "Failed to create MIDI clip" : r.error);
    auto result = choc::value::createObject ("");
    result.setMember ("clipId", r.clipId);
    return result;
}

static choc::value::Value toolPlay(const choc::value::Value&)
{
    AppModel::instance().sequencer().engine()->startPlayback();
    auto result = choc::value::createObject ("");
    result.setMember ("playing", true);
    return result;
}

static choc::value::Value toolStop(const choc::value::Value&)
{
    AppModel::instance().sequencer().engine()->stopPlayback();
    auto result = choc::value::createObject ("playing");
    result.setMember ("playing", false);
    return result;
}

static choc::value::Value toolGetPresets(const choc::value::Value& args)
{
    auto instanceId = getIntArg (args, "instanceId");
    if (instanceId < 0)
        throw std::invalid_argument ("instanceId is required");

    auto& sequencer = AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance (instanceId);

    if (! instance)
        throw std::runtime_error ("Plugin instance not found");

    auto presets = instance->presetMetadataList();
    auto arr = choc::value::createEmptyArray();

    for (const auto& p : presets)
    {
        auto obj = choc::value::createObject ("");
        obj.setMember ("index", static_cast<int32_t>(p.index));
        obj.setMember ("name", p.name);
        obj.setMember ("bank", static_cast<int32_t>(p.bank));
        arr.addArrayElement (obj);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("presets", arr);
    return result;
}

static choc::value::Value toolLoadPreset(const choc::value::Value& args)
{
    auto instanceId = getIntArg (args, "instanceId");
    auto presetIndex = getIntArg (args, "presetIndex");

    if (instanceId < 0 || presetIndex < 0)
        throw std::invalid_argument ("instanceId and presetIndex are required");

    auto& sequencer = AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance (instanceId);

    if (! instance)
        throw std::runtime_error ("Plugin instance not found");

    instance->loadPreset (presetIndex);
    return choc::value::createObject ("");
}


// ─────────────────────────────────────────────────
//  McpServer::Impl
// ─────────────────────────────────────────────────

struct McpServer::Impl {
    // ── Transport config ──────────────────────────────────────────────────
    McpConnectionMode mode_          = McpConnectionMode::Client;
    int               port_          = 37373;
    std::string       relayUrl_;
    bool              autoReconnect_ = true;

    std::atomic<McpConnectionState> connState_{McpConnectionState::Idle};
    mutable std::mutex statusMutex_;
    std::string        statusMsg_;

    // ── Transport objects ─────────────────────────────────────────────────
#ifdef UAPMD_MCP_HAS_HTTP_SERVER
    httplib::Server httpServer_;
    std::thread     httpThread_;
#endif
#ifndef __EMSCRIPTEN__
    ix::WebSocket wsClient_;
#endif

    // ── JS runtime (lazily created, shared between both transports) ───────
    std::unique_ptr<UapmdJSRuntime> jsRuntime_;

    // ── Main-thread dispatch queue ────────────────────────────────────────
    struct PendingCall {
        std::function<std::string()> handler;
        std::promise<std::string>    promise;
    };
    std::mutex queueMutex_;
    std::queue<std::shared_ptr<PendingCall>> queue_;

    // Post work to the main thread and block until it's processed.
    // On Wasm the JS caller is already on the main thread, so call directly.
    // On desktop/mobile the caller is a background thread (httplib / IXWebSocket).
    std::string dispatchToMainThread(std::function<std::string()> fn)
    {
#ifdef __EMSCRIPTEN__
        return fn();
#else
        auto call = std::make_shared<PendingCall>();
        call->handler = std::move (fn);
        auto future = call->promise.get_future();
        {
            std::lock_guard lock (queueMutex_);
            queue_.push (call);
        }
        return future.get();
#endif
    }

    // Called from main thread each frame.
    void processQueue()
    {
        while (true)
        {
            std::shared_ptr<PendingCall> call;
            {
                std::lock_guard lock (queueMutex_);
                if (queue_.empty()) break;
                call = queue_.front();
                queue_.pop();
            }
            try {
                call->promise.set_value (call->handler());
            } catch (...) {
                call->promise.set_exception (std::current_exception());
            }
        }
    }

    void setStatus(McpConnectionState state, std::string msg)
    {
        connState_ = state;
        std::lock_guard lock (statusMutex_);
        statusMsg_ = std::move (msg);
    }

    // ---- MCP protocol handlers ----

    std::string handleRequest(const std::string& body)
    {
        choc::value::Value msg;
        try {
            msg = choc::json::parse (body);
        } catch (...) {
            return jsonRpcError ({}, -32700, "Parse error");
        }

        auto id = choc::value::Value{};
        if (msg.isObject() && msg.hasObjectMember ("id"))
            id = choc::value::Value (msg["id"]);

        if (!msg.isObject() || !msg.hasObjectMember ("method"))
            return jsonRpcError (id, -32600, "Invalid Request");

        const auto method = msg["method"].get<std::string>();

        // Notifications (no id field) require no response.
        if (!msg.hasObjectMember ("id"))
            return {};

        if (method == "initialize")   return handleInitialize (id);
        if (method == "tools/list")   return handleToolsList (id);
        if (method == "tools/call")   return handleToolsCall (id, msg);

        return jsonRpcError (id, -32601, "Method not found: " + method);
    }

    std::string handleInitialize(const choc::value::Value& id)
    {
        auto result = choc::value::createObject ("");
        result.setMember ("protocolVersion", std::string ("2024-11-05"));

        auto caps = choc::value::createObject ("");
        caps.setMember ("tools", choc::value::createObject (""));
        result.setMember ("capabilities", caps);

        auto info = choc::value::createObject ("");
        info.setMember ("name", std::string ("uapmd"));
        info.setMember ("version", std::string ("1.0.0"));
        result.setMember ("serverInfo", info);

        return jsonRpcResult (id, result);
    }

    std::string handleToolsList(const choc::value::Value& id)
    {
        auto result = choc::value::createObject ("");
        result.setMember ("tools", buildToolDefinitions());
        return jsonRpcResult (id, result);
    }

    // Lazily initialise the JS runtime and bootstrap all AppJsLib scripts.
    // Safe to call multiple times — no-op after the first call.
    void ensureJSRuntime()
    {
        if (jsRuntime_)
            return;
        jsRuntime_ = std::make_unique<UapmdJSRuntime>();
        for (auto& [filename, data] : ResEmbed::getCategory ("AppJsLib"))
        {
            std::string src (reinterpret_cast<const char*> (data.data()), data.size());
            try { jsRuntime_->context().evaluateExpression (src); }
            catch (const std::exception& e) {
                std::cerr << "[MCP] Failed to bootstrap " << filename << ": " << e.what() << std::endl;
            }
        }
    }

    // Execute a JS snippet and return the string representation of the result.
    // Throws on evaluation error.
    std::string evalScript(const std::string& code)
    {
        ensureJSRuntime();
        auto evalResult = jsRuntime_->context().evaluateExpression (code);
        return evalResult.isVoid() ? std::string ("undefined")
                                   : choc::json::toString (evalResult);
    }

    std::string handleToolsCall(const choc::value::Value& id, const choc::value::Value& msg)
    {
        if (!msg.hasObjectMember ("params"))
            return jsonRpcError (id, -32602, "Missing params");

        const auto& params = msg["params"];
        if (!params.hasObjectMember ("name"))
            return jsonRpcError (id, -32602, "Missing tool name");

        const auto toolName = params["name"].get<std::string>();

        // Extract arguments as an owned Value (not a view into msg).
        auto args = params.hasObjectMember ("arguments")
            ? choc::value::Value (params["arguments"])
            : choc::value::createObject ("");

        try
        {
            choc::value::Value toolResult;

            if      (toolName == "list_plugins")        toolResult = toolListPlugins (args);
            else if (toolName == "list_tracks")         toolResult = toolListTracks (args);
            else if (toolName == "create_track")        toolResult = toolCreateTrack (args);
            else if (toolName == "add_plugin_to_track") toolResult = toolAddPluginToTrack (args);
            else if (toolName == "get_timeline_state")  toolResult = toolGetTimelineState (args);
            else if (toolName == "set_tempo")           toolResult = toolSetTempo (args);
            else if (toolName == "list_clips")               toolResult = toolListClips (args);
            else if (toolName == "get_clip_audio_events")    toolResult = toolGetClipAudioEvents (args);
            else if (toolName == "set_clip_audio_events")    toolResult = toolSetClipAudioEvents (args);
            else if (toolName == "get_master_markers")       toolResult = toolGetMasterMarkers (args);
            else if (toolName == "set_master_markers")       toolResult = toolSetMasterMarkers (args);
            else if (toolName == "add_midi_clip")            toolResult = toolAddMidiClip (args);
            else if (toolName == "remove_clip")              toolResult = toolRemoveClip (args);
            else if (toolName == "get_clip_ump_events")      toolResult = toolGetClipUmpEvents (args);
            else if (toolName == "add_ump_event")            toolResult = toolAddUmpEvent (args);
            else if (toolName == "remove_ump_event")         toolResult = toolRemoveUmpEvent (args);
            else if (toolName == "create_empty_midi_clip")   toolResult = toolCreateEmptyMidiClip (args);
            else if (toolName == "play")                     toolResult = toolPlay (args);
            else if (toolName == "stop")                     toolResult = toolStop (args);
            else if (toolName == "get_presets")             toolResult = toolGetPresets (args);
            else if (toolName == "load_preset")             toolResult = toolLoadPreset (args);
            else if (toolName == "run_script")
            {
                auto code = getStringArg (args, "code");
                if (code.empty())
                    throw std::invalid_argument ("code is required");
                auto output = evalScript (code);
                toolResult = choc::value::createObject ("");
                toolResult.setMember ("output", output);
            }
            else
                return jsonRpcError (id, -32601, "Unknown tool: " + toolName);

            auto result = choc::value::createObject ("");
            result.setMember ("content", textContent (choc::json::toString (toolResult)));
            return jsonRpcResult (id, result);
        }
        catch (const std::exception& e)
        {
            auto result = choc::value::createObject ("");
            result.setMember ("content", textContent (std::string ("Error: ") + e.what()));
            result.setMember ("isError", true);
            return jsonRpcResult (id, result);
        }
    }
};

// ─────────────────────────────────────────────────
//  McpServer public API
// ─────────────────────────────────────────────────

#ifndef __EMSCRIPTEN__   // ── Desktop / mobile transport ──────────────────

McpServer::McpServer(int port)
    : impl_ (std::make_unique<Impl>())
{
    impl_->mode_ = McpConnectionMode::Server;
    impl_->port_ = port;
}

McpServer::McpServer(std::string relayUrl, bool autoReconnect)
    : impl_ (std::make_unique<Impl>())
{
    impl_->mode_          = McpConnectionMode::Client;
    impl_->relayUrl_      = std::move (relayUrl);
    impl_->autoReconnect_ = autoReconnect;
}

McpServer::~McpServer()
{
    stop();
}

void McpServer::start()
{
#ifdef UAPMD_MCP_HAS_HTTP_SERVER
    if (impl_->mode_ == McpConnectionMode::Server)
    {
        impl_->httpServer_.Post ("/mcp", [this] (const httplib::Request& req, httplib::Response& res)
        {
            const auto body = req.body;
            std::string response = impl_->dispatchToMainThread ([this, body] {
                return impl_->handleRequest (body);
            });
            if (response.empty())
                res.status = 202;
            else
                res.set_content (response, "application/json");
        });

        impl_->httpServer_.Post ("/eval", [this] (const httplib::Request& req, httplib::Response& res)
        {
            const auto code = req.body;
            try {
                std::string output = impl_->dispatchToMainThread ([this, code] {
                    return impl_->evalScript (code);
                });
                res.set_content (output, "text/plain");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content (e.what(), "text/plain");
            }
        });

        const auto port = impl_->port_;
        impl_->httpThread_ = std::thread ([this, port] {
            impl_->setStatus (McpConnectionState::Connected,
                              "Listening on port " + std::to_string (port));
            std::cout << "[MCP] Listening on http://127.0.0.1:" << port
                      << "/mcp  (JS eval: /eval)" << std::endl;
            impl_->httpServer_.listen ("127.0.0.1", port);
            impl_->setStatus (McpConnectionState::Idle, "Stopped");
        });
        return;
    }
#endif

    // ── Client mode — outbound WebSocket ─────────────────────────────────
    impl_->wsClient_.setUrl (impl_->relayUrl_);

    if (impl_->autoReconnect_)
        impl_->wsClient_.enableAutomaticReconnection();

    impl_->wsClient_.setOnMessageCallback ([this] (const ix::WebSocketMessagePtr& msg)
    {
        switch (msg->type)
        {
        case ix::WebSocketMessageType::Open:
            impl_->setStatus (McpConnectionState::Connected,
                              "Connected to " + impl_->relayUrl_);
            break;

        case ix::WebSocketMessageType::Close:
            impl_->setStatus (impl_->autoReconnect_ ? McpConnectionState::Connecting
                                                     : McpConnectionState::Idle,
                              impl_->autoReconnect_ ? "Reconnecting..." : "Disconnected");
            break;

        case ix::WebSocketMessageType::Error:
            impl_->setStatus (McpConnectionState::Error,
                              "Error: " + msg->errorInfo.reason);
            break;

        case ix::WebSocketMessageType::Message:
        {
            const auto body = msg->str;
            std::string response = impl_->dispatchToMainThread ([this, body] {
                return impl_->handleRequest (body);
            });
            if (!response.empty())
                impl_->wsClient_.sendText (response);
            break;
        }

        default:
            break;
        }
    });

    impl_->setStatus (McpConnectionState::Connecting,
                      "Connecting to " + impl_->relayUrl_);
    impl_->wsClient_.start();
}  // end start()

void McpServer::stop()
{
#ifdef UAPMD_MCP_HAS_HTTP_SERVER
    if (impl_->mode_ == McpConnectionMode::Server)
    {
        impl_->httpServer_.stop();
        if (impl_->httpThread_.joinable())
            impl_->httpThread_.join();
        return;
    }
#endif
    impl_->wsClient_.stop();
    impl_->setStatus (McpConnectionState::Idle, "Disconnected");
}

McpConnectionMode McpServer::mode() const          { return impl_->mode_; }
McpConnectionState McpServer::connectionState() const { return impl_->connState_.load(); }
int McpServer::port() const                        { return impl_->port_; }
void McpServer::processMainThreadQueue()           { impl_->processQueue(); }

std::string McpServer::statusMessage() const
{
    std::lock_guard lock (impl_->statusMutex_);
    return impl_->statusMsg_;
}

#else  // ── Wasm transport ──────────────────────────────────────────────────

// On Wasm, McpServer is a thin wrapper around Impl (for the JS runtime and the
// dispatch queue).  The protocol is always reachable via the C exports below.
// No sockets or threads are involved.

// Opaque delegates — avoids naming the private McpServer::Impl type at file scope.
static std::function<std::string(const char*)> gWasmCall;
static std::function<std::string(const char*)> gWasmEval;

McpServer::McpServer(int port)
    : impl_ (std::make_unique<Impl>())
{
    impl_->mode_ = McpConnectionMode::Server;
    impl_->port_ = port;
}

McpServer::McpServer(std::string relayUrl, bool autoReconnect)
    : impl_ (std::make_unique<Impl>())
{
    impl_->mode_          = McpConnectionMode::Client;
    impl_->relayUrl_      = std::move (relayUrl);
    impl_->autoReconnect_ = autoReconnect;
}

McpServer::~McpServer()
{
    gWasmCall = nullptr;
    gWasmEval = nullptr;
}

void McpServer::start()
{
    auto* p = impl_.get();
    gWasmCall = [p](const char* req)  { return p->handleRequest (req ? req : ""); };
    gWasmEval = [p](const char* code) { return p->evalScript    (code ? code : ""); };
    impl_->setStatus (McpConnectionState::Connected, "Active (JS export)");
}

void McpServer::stop()
{
    gWasmCall = nullptr;
    gWasmEval = nullptr;
    impl_->setStatus (McpConnectionState::Idle, "Stopped");
}

McpConnectionMode  McpServer::mode()            const { return impl_->mode_; }
McpConnectionState McpServer::connectionState() const { return impl_->connState_.load(); }
int                McpServer::port()            const { return impl_->port_; }
void               McpServer::processMainThreadQueue() { impl_->processQueue(); }

std::string McpServer::statusMessage() const
{
    std::lock_guard lock (impl_->statusMutex_);
    return impl_->statusMsg_;
}

// ── Wasm C exports ────────────────────────────────────────────────────────────
// Call from JS:
//   const resp = Module.ccall('uapmd_mcp_call','string',['string'],[jsonRpcRequest]);
//   const out  = Module.ccall('uapmd_eval',    'string',['string'],[jsCode]);

extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* uapmd_mcp_call(const char* requestJson)
{
    if (!gWasmCall || !requestJson)
        return "";
    static std::string response;
    response = gWasmCall (requestJson);
    return response.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* uapmd_eval(const char* code)
{
    if (!gWasmEval || !code)
        return "undefined";
    static std::string result;
    try {
        result = gWasmEval (code);
    } catch (const std::exception& e) {
        result = std::string ("Error: ") + e.what();
    }
    return result.c_str();
}

} // extern "C"

#endif // __EMSCRIPTEN__

} // namespace uapmd
