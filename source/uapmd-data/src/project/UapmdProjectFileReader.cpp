#include <choc/text/choc_JSON.h>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <iostream>
#include "uapmd-data/uapmd-data.hpp"
#include "UapmdAudioPluginFullDAGraphData.hpp"
#include "UapmdProjectPluginListGraphData.hpp"

namespace uapmd {
    namespace {
        constexpr double kLegacyOffsetSampleRate = 48000.0;

        AudioPluginGraphEndpointType parseGraphEndpointType(std::string_view value) {
            if (value == "graph_input")
                return AudioPluginGraphEndpointType::GraphInput;
            if (value == "graph_output")
                return AudioPluginGraphEndpointType::GraphOutput;
            return AudioPluginGraphEndpointType::Plugin;
        }

        AudioPluginGraphBusType parseGraphBusType(std::string_view value) {
            return value == "event" ? AudioPluginGraphBusType::Event : AudioPluginGraphBusType::Audio;
        }

        std::string parseGraphType(std::string_view value) {
            if (value.empty())
                return {};
            return std::string(value);
        }

        double legacyOffsetSamplesToSeconds(int64_t samples) {
            return static_cast<double>(samples) / kLegacyOffsetSampleRate;
        }

        AudioWarpReferenceType parseAudioWarpReferenceType(const choc::value::ValueView& warpObj) {
            if (!warpObj.hasObjectMember("reference_type"))
                return warpObj.hasObjectMember("marker_id")
                    ? AudioWarpReferenceType::ClipMarker
                    : AudioWarpReferenceType::Manual;

            auto type = std::string(warpObj["reference_type"].getString());
            if (type == "clip_start")
                return AudioWarpReferenceType::ClipStart;
            if (type == "clip_end")
                return AudioWarpReferenceType::ClipEnd;
            if (type == "clip_marker")
                return AudioWarpReferenceType::ClipMarker;
            if (type == "master_marker")
                return AudioWarpReferenceType::MasterMarker;
            return AudioWarpReferenceType::Manual;
        }
    }

    // Helper to resolve clip anchors after all tracks and clips are loaded
    class AnchorResolver {
        UapmdProjectData* project_;
        std::map<std::string, UapmdClipDataReferencible*> anchor_map_;

    public:
        explicit AnchorResolver(UapmdProjectData* project) : project_(project) {
            // Build anchor map - tracks are indexed by their position
            size_t track_idx = 0;
            for (auto track : project_->tracks()) {
                anchor_map_["track_" + std::to_string(track_idx)] = track;

                // Clips within each track
                size_t clip_idx = 0;
                for (auto& clip : track->clips()) {
                    std::string clip_id = "track_" + std::to_string(track_idx) +
                                         "_clip_" + std::to_string(clip_idx);
                    anchor_map_[clip_id] = clip.get();
                    ++clip_idx;
                }
                ++track_idx;
            }

            // Master track clips
            size_t clip_idx = 0;
            for (auto& clip : project_->masterTrack()->clips()) {
                std::string clip_id = "master_clip_" + std::to_string(clip_idx);
                anchor_map_[clip_id] = clip.get();
                ++clip_idx;
            }
        }

        UapmdClipDataReferencible* resolve(const std::string& anchor) {
            auto it = anchor_map_.find(anchor);
            return it != anchor_map_.end() ? it->second : nullptr;
        }

        // Check if an anchor is valid (exists and doesn't create cycles)
        bool isValidAnchor(const std::string& anchor, UapmdClipDataReferencible* clipRef) {
            // Check if anchor exists
            auto* anchorPtr = resolve(anchor);
            if (!anchorPtr)
                return false;

            // Check for cycles using DFS
            std::set<UapmdClipDataReferencible*> visited;
            return !hasCycle(anchorPtr, clipRef, visited);
        }

    private:
        bool hasCycle(UapmdClipDataReferencible* current, UapmdClipDataReferencible* target,
                     std::set<UapmdClipDataReferencible*>& visited) {
            // If we've reached the target clip, we have a cycle
            if (current == target)
                return true;

            // If already visited, no cycle on this path
            if (visited.count(current))
                return false;

            visited.insert(current);

            // Only clips can have anchors; tracks always return 0 for position
            auto* clip = dynamic_cast<UapmdProjectClipData*>(current);
            if (clip) {
                auto pos = clip->position();
                if (pos.anchor && hasCycle(pos.anchor, target, visited))
                    return true;
            }

            return false;
        }
    };

    static std::unique_ptr<UapmdProjectClipData> parseClip(
        const choc::value::ValueView& clipObj,
        const std::string& anchorId,
        AnchorResolver* resolver)
    {
        auto clip = UapmdProjectClipData::create();

        // Parse position
        UapmdTimelinePosition pos{};
        if (clipObj.hasObjectMember("anchor")) {
            auto anchor_view = clipObj["anchor"].getString();
            std::string anchor_str(anchor_view);
            pos.anchor = resolver ? resolver->resolve(anchor_str) : nullptr;
        }
        pos.samples = clipObj["position_samples"].getWithDefault<uint64_t>(0);
        clip->position(pos);

        // Parse file reference
        if (clipObj.hasObjectMember("file")) {
            auto file_view = clipObj["file"].getString();
            clip->file(std::string(file_view));
        }

        // Parse MIME type
        if (clipObj.hasObjectMember("mime_type")) {
            auto mime_view = clipObj["mime_type"].getString();
            clip->mimeType(std::string(mime_view));
        }

        // Parse clip type
        if (clipObj.hasObjectMember("clip_type")) {
            auto type_view = clipObj["clip_type"].getString();
            clip->clipType(std::string(type_view));
        }

        // Parse MIDI-specific metadata
        if (clipObj.hasObjectMember("tick_resolution")) {
            clip->tickResolution(clipObj["tick_resolution"].getWithDefault<uint32_t>(480));
        }
        if (clipObj.hasObjectMember("nrpn_to_parameter_mapping"))
            clip->nrpnToParameterMapping(clipObj["nrpn_to_parameter_mapping"].getWithDefault<bool>(false));
        if (clipObj.hasObjectMember("markers") && clipObj["markers"].isArray()) {
            std::vector<ClipMarker> markers;
            for (const auto& markerObj : clipObj["markers"]) {
                ClipMarker marker;
                if (markerObj.hasObjectMember("id"))
                    marker.markerId = std::string(markerObj["id"].getString());
                marker.clipPositionOffset = markerObj.hasObjectMember("position_offset_seconds")
                    ? markerObj["position_offset_seconds"].getWithDefault<double>(0.0)
                    : legacyOffsetSamplesToSeconds(markerObj.hasObjectMember("position_offset_samples")
                        ? markerObj["position_offset_samples"].getWithDefault<int64_t>(0)
                        : markerObj["position_samples"].getWithDefault<int64_t>(0));
                marker.referenceType = markerObj.hasObjectMember("reference_type")
                    ? parseAudioWarpReferenceType(markerObj)
                    : AudioWarpReferenceType::ClipStart;
                if (markerObj.hasObjectMember("reference_clip_id"))
                    marker.referenceClipId = std::string(markerObj["reference_clip_id"].getString());
                if (markerObj.hasObjectMember("reference_marker_id"))
                    marker.referenceMarkerId = std::string(markerObj["reference_marker_id"].getString());
                if (markerObj.hasObjectMember("name"))
                    marker.name = std::string(markerObj["name"].getString());
                markers.push_back(std::move(marker));
            }
            clip->markers(std::move(markers));
        }
        if (clipObj.hasObjectMember("audio_warps") && clipObj["audio_warps"].isArray()) {
            std::vector<AudioWarpPoint> audioWarps;
            for (const auto& warpObj : clipObj["audio_warps"]) {
                AudioWarpPoint warp;
                warp.clipPositionOffset = warpObj.hasObjectMember("offset_seconds")
                    ? warpObj["offset_seconds"].getWithDefault<double>(0.0)
                    : legacyOffsetSamplesToSeconds(warpObj.hasObjectMember("clip_position_offset_samples")
                        ? warpObj["clip_position_offset_samples"].getWithDefault<int64_t>(0)
                        : warpObj.hasObjectMember("offset_samples")
                        ? warpObj["offset_samples"].getWithDefault<int64_t>(0)
                        : warpObj.hasObjectMember("source_offset_samples")
                        ? warpObj["source_offset_samples"].getWithDefault<int64_t>(0)
                        : warpObj.hasObjectMember("source_position_samples")
                        ? warpObj["source_position_samples"].getWithDefault<int64_t>(0)
                        : warpObj["clip_position_samples"].getWithDefault<int64_t>(0));
                warp.speedRatio = warpObj["speed_ratio"].getWithDefault<double>(1.0);
                warp.referenceType = parseAudioWarpReferenceType(warpObj);
                if (warpObj.hasObjectMember("reference_clip_id"))
                    warp.referenceClipId = std::string(warpObj["reference_clip_id"].getString());
                if (warpObj.hasObjectMember("reference_marker_id"))
                    warp.referenceMarkerId = std::string(warpObj["reference_marker_id"].getString());
                if (warp.referenceMarkerId.empty() && warpObj.hasObjectMember("marker_id"))
                    warp.referenceMarkerId = std::string(warpObj["marker_id"].getString());
                audioWarps.push_back(std::move(warp));
            }
            clip->audioWarps(std::move(audioWarps));
        }

        return clip;
    }

    static std::unique_ptr<UapmdProjectPluginGraphData> parsePluginGraph(
        const choc::value::ValueView& graphObj)
    {
        const bool hasPlugins = graphObj.hasObjectMember("plugins") && graphObj["plugins"].isArray();
        const bool hasConnections = graphObj.hasObjectMember("connections") && graphObj["connections"].isArray();
        bool hasInlineGraphData = hasPlugins || hasConnections;

        std::unique_ptr<UapmdProjectPluginGraphData> graph;
        if (hasConnections)
            graph = UapmdAudioPluginFullDAGraphData::create();
        else if (hasPlugins)
            graph = UapmdProjectPluginListGraphData::create();
        else
            graph = UapmdProjectPluginGraphData::create();

        if (graphObj.hasObjectMember("graph_type"))
            graph->graphType(parseGraphType(graphObj["graph_type"].getString()));
        else if (graphObj.hasObjectMember("graph_kind"))
            graph->graphType(parseGraphType(graphObj["graph_kind"].getString()));

        if (graphObj.hasObjectMember("external_file")) {
            auto file_view = graphObj["external_file"].getString();
            graph->externalFile(std::string(file_view));
        }

        if (hasPlugins) {
            auto* pluginListGraph = dynamic_cast<UapmdProjectPluginListGraphData*>(graph.get());
            for (const auto& pluginObj : graphObj["plugins"]) {
                UapmdProjectPluginNodeData node;
                if (pluginObj.hasObjectMember("plugin_id"))
                    node.plugin_id = std::string(pluginObj["plugin_id"].getString());
                if (pluginObj.hasObjectMember("format"))
                    node.format = std::string(pluginObj["format"].getString());
                if (pluginObj.hasObjectMember("display_name"))
                    node.display_name = std::string(pluginObj["display_name"].getString());
                if (pluginObj.hasObjectMember("state_file"))
                    node.state_file = std::string(pluginObj["state_file"].getString());
                if (pluginObj.hasObjectMember("group_index"))
                    node.group_index = pluginObj["group_index"].getWithDefault<int32_t>(-1);
                pluginListGraph->addPlugin(std::move(node));
            }
        }

        if (hasConnections) {
            auto* dagGraph = dynamic_cast<UapmdAudioPluginFullDAGraphData*>(graph.get());
            if (!dagGraph) {
                auto converted = UapmdAudioPluginFullDAGraphData::create();
                converted->graphType(graph->graphType());
                converted->externalFile(graph->externalFile());
                if (auto* pluginListGraph = dynamic_cast<UapmdProjectPluginListGraphData*>(graph.get()))
                    converted->setPlugins(pluginListGraph->plugins());
                graph = std::move(converted);
                dagGraph = dynamic_cast<UapmdAudioPluginFullDAGraphData*>(graph.get());
            }
            for (const auto& connectionObj : graphObj["connections"]) {
                UapmdProjectPluginGraphConnectionData connection;
                if (connectionObj.hasObjectMember("id"))
                    connection.id = connectionObj["id"].getWithDefault<int64_t>(0);
                if (connectionObj.hasObjectMember("bus_type"))
                    connection.bus_type = parseGraphBusType(connectionObj["bus_type"].getString());
                if (connectionObj.hasObjectMember("source") && connectionObj["source"].isObject()) {
                    auto endpointObj = connectionObj["source"];
                    if (endpointObj.hasObjectMember("type"))
                        connection.source.type = parseGraphEndpointType(endpointObj["type"].getString());
                    if (endpointObj.hasObjectMember("plugin_index"))
                        connection.source.plugin_index = endpointObj["plugin_index"].getWithDefault<int32_t>(-1);
                    if (endpointObj.hasObjectMember("bus_index"))
                        connection.source.bus_index = endpointObj["bus_index"].getWithDefault<uint32_t>(0);
                }
                if (connectionObj.hasObjectMember("target") && connectionObj["target"].isObject()) {
                    auto endpointObj = connectionObj["target"];
                    if (endpointObj.hasObjectMember("type"))
                        connection.target.type = parseGraphEndpointType(endpointObj["type"].getString());
                    if (endpointObj.hasObjectMember("plugin_index"))
                        connection.target.plugin_index = endpointObj["plugin_index"].getWithDefault<int32_t>(-1);
                    if (endpointObj.hasObjectMember("bus_index"))
                        connection.target.bus_index = endpointObj["bus_index"].getWithDefault<uint32_t>(0);
                }
                dagGraph->addConnection(std::move(connection));
            }
        }

        // Legacy embedded graph data is treated as simple linear on load.
        if (graph->externalFile().empty() && hasInlineGraphData)
            graph->graphType({});

        return graph;
    }

    static std::unique_ptr<UapmdProjectTrackData> parseTrack(
        const choc::value::ValueView& trackObj,
        size_t trackIndex)
    {
        auto track = UapmdProjectTrackData::create();

        // Parse plugin graph
        if (trackObj.hasObjectMember("graph"))
            track->graph(parsePluginGraph(trackObj["graph"]));

        if (trackObj.hasObjectMember("markers") && trackObj["markers"].isArray()) {
            std::vector<ClipMarker> markers;
            for (const auto& markerObj : trackObj["markers"]) {
                ClipMarker marker;
                if (markerObj.hasObjectMember("id"))
                    marker.markerId = std::string(markerObj["id"].getString());
                marker.clipPositionOffset = markerObj.hasObjectMember("position_offset_seconds")
                    ? markerObj["position_offset_seconds"].getWithDefault<double>(0.0)
                    : legacyOffsetSamplesToSeconds(markerObj.hasObjectMember("position_offset_samples")
                        ? markerObj["position_offset_samples"].getWithDefault<int64_t>(0)
                        : markerObj["position_samples"].getWithDefault<int64_t>(0));
                marker.referenceType = markerObj.hasObjectMember("reference_type")
                    ? parseAudioWarpReferenceType(markerObj)
                    : AudioWarpReferenceType::ClipStart;
                if (markerObj.hasObjectMember("reference_clip_id"))
                    marker.referenceClipId = std::string(markerObj["reference_clip_id"].getString());
                if (markerObj.hasObjectMember("reference_marker_id"))
                    marker.referenceMarkerId = std::string(markerObj["reference_marker_id"].getString());
                if (markerObj.hasObjectMember("name"))
                    marker.name = std::string(markerObj["name"].getString());
                markers.push_back(std::move(marker));
            }
            track->markers(std::move(markers));
        }

        // Parse clips (first pass - without anchor resolution)
        if (trackObj.hasObjectMember("clips") && trackObj["clips"].isArray()) {
            size_t clipIdx = 0;
            for (const auto& clipObj : trackObj["clips"]) {
                std::string anchorId = "track_" + std::to_string(trackIndex) +
                                      "_clip_" + std::to_string(clipIdx);
                track->clips().push_back(parseClip(clipObj, anchorId, nullptr));
                ++clipIdx;
            }
        }

        return track;
    }

    std::unique_ptr<UapmdProjectData> UapmdProjectDataReader::read(std::filesystem::path file) {
        // Read file contents
        std::ifstream ifs(file);
        if (!ifs)
            return nullptr;

        std::stringstream buffer;
        buffer << ifs.rdbuf();
        std::string content = buffer.str();

        // Parse JSON
        auto root = choc::json::parse(content);
        auto project = UapmdProjectData::create();

        // Parse tracks
        if (root.hasObjectMember("tracks") && root["tracks"].isArray()) {
            size_t trackIndex = 0;
            for (const auto& trackObj : root["tracks"]) {
                project->addTrack(parseTrack(trackObj, trackIndex));
                ++trackIndex;
            }
        }

        // Parse master track
        if (root.hasObjectMember("master_track")) {
            auto masterTrack = parseTrack(root["master_track"], 999999);
            // Note: master track is already created in constructor
            // We need to populate it instead
            auto* master = dynamic_cast<UapmdProjectTrackData*>(project->masterTrack());
            if (master) {
                if (root["master_track"].hasObjectMember("graph"))
                    master->graph(parsePluginGraph(root["master_track"]["graph"]));
                master->markers(masterTrack->markers());
                for (auto& clip : masterTrack->clips())
                    master->clips().push_back(std::move(clip));
            }
        }

        // Second pass: resolve anchors for all clips and validate them
        AnchorResolver resolver(project.get());

        // Re-parse clips with anchor resolution and validation
        if (root.hasObjectMember("tracks") && root["tracks"].isArray()) {
            size_t trackIndex = 0;
            for (const auto& trackObj : root["tracks"]) {
                auto& track = project->tracks()[trackIndex];
                auto& clips = track->clips();

                if (trackObj.hasObjectMember("clips") && trackObj["clips"].isArray()) {
                    size_t clipIdx = 0;
                    std::vector<size_t> invalidClipIndices;

                    for (const auto& clipObj : trackObj["clips"]) {
                        auto* clipImpl = dynamic_cast<UapmdProjectClipData*>(clips[clipIdx].get());

                        if (clipImpl && clipObj.hasObjectMember("anchor")) {
                            auto anchor_view = clipObj["anchor"].getString();
                            std::string anchor_str(anchor_view);

                            // Validate anchor
                            if (!resolver.isValidAnchor(anchor_str, clipImpl)) {
                                auto* anchorPtr = resolver.resolve(anchor_str);
                                if (!anchorPtr) {
                                    std::cerr << "Warning: Invalid anchor '" << anchor_str
                                              << "' in track " << trackIndex << " clip " << clipIdx
                                              << " - anchor not found. Clip will be removed.\n";
                                } else {
                                    std::cerr << "Warning: Invalid anchor '" << anchor_str
                                              << "' in track " << trackIndex << " clip " << clipIdx
                                              << " - creates recursive reference. Clip will be removed.\n";
                                }
                                invalidClipIndices.push_back(clipIdx);
                            } else {
                                // Valid anchor - update position
                                UapmdTimelinePosition pos = clipImpl->position();
                                pos.anchor = resolver.resolve(anchor_str);
                                clipImpl->position(pos);
                            }
                        }
                        ++clipIdx;
                    }

                    // Remove invalid clips (in reverse order to maintain indices)
                    for (auto it = invalidClipIndices.rbegin(); it != invalidClipIndices.rend(); ++it)
                        clips.erase(clips.begin() + *it);
                }
                ++trackIndex;
            }
        }

        // Validate master track clips
        if (root.hasObjectMember("master_track")) {
            if (auto* master = project->masterTrack()) {
                auto& clips = master->clips();
                auto masterTrackObj = root["master_track"];

                if (masterTrackObj.hasObjectMember("clips") && masterTrackObj["clips"].isArray()) {
                    size_t clipIdx = 0;
                    std::vector<size_t> invalidClipIndices;

                    if (auto clipsArr = masterTrackObj["clips"]; clipsArr.size() == 0) {
                        for (const auto& clipObj : clipsArr) {
                            auto* clipImpl = clips[clipIdx].get();

                            if (clipImpl && clipObj.hasObjectMember("anchor")) {
                                auto anchor_view = clipObj["anchor"].getString();
                                std::string anchor_str(anchor_view);

                                // Validate anchor
                                if (!resolver.isValidAnchor(anchor_str, clipImpl)) {
                                    auto* anchorPtr = resolver.resolve(anchor_str);
                                    if (!anchorPtr) {
                                        std::cerr << "Warning: Invalid anchor '" << anchor_str
                                                  << "' in master track clip " << clipIdx
                                                  << " - anchor not found. Clip will be removed.\n";
                                    } else {
                                        std::cerr << "Warning: Invalid anchor '" << anchor_str
                                                  << "' in master track clip " << clipIdx
                                                  << " - creates recursive reference. Clip will be removed.\n";
                                    }
                                    invalidClipIndices.push_back(clipIdx);
                                } else {
                                    // Valid anchor - update position
                                    UapmdTimelinePosition pos = clipImpl->position();
                                    pos.anchor = resolver.resolve(anchor_str);
                                    clipImpl->position(pos);
                                }
                            }
                            ++clipIdx;
                        }
                    }

                    // Remove invalid clips (in reverse order to maintain indices)
                    for (auto it = invalidClipIndices.rbegin(); it != invalidClipIndices.rend(); ++it)
                        clips.erase(clips.begin() + *it);
                }
            }
        }

        return project;
    }
}
