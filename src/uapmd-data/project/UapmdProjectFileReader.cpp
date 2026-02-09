#include <choc/text/choc_JSON.h>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <iostream>
#include "uapmd-data/uapmd-data.hpp"

namespace uapmd {
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
        if (clipObj.hasObjectMember("tempo")) {
            clip->tempo(clipObj["tempo"].getWithDefault<double>(120.0));
        }

        return clip;
    }

    static std::unique_ptr<UapmdProjectPluginGraphData> parsePluginGraph(
        const choc::value::ValueView& graphObj)
    {
        auto graph = UapmdProjectPluginGraphData::create();

        if (graphObj.hasObjectMember("external_file")) {
            auto file_view = graphObj["external_file"].getString();
            graph->externalFile(std::string(file_view));
        }

        if (graphObj.hasObjectMember("plugins") && graphObj["plugins"].isArray()) {
            for (const auto& pluginObj : graphObj["plugins"]) {
                UapmdProjectPluginNodeData node;
                if (pluginObj.hasObjectMember("plugin_id"))
                    node.plugin_id = std::string(pluginObj["plugin_id"].getString());
                if (pluginObj.hasObjectMember("format"))
                    node.format = std::string(pluginObj["format"].getString());
                if (pluginObj.hasObjectMember("state_file"))
                    node.state_file = std::string(pluginObj["state_file"].getString());
                graph->addPlugin(std::move(node));
            }
        }

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
            if (master && root["master_track"].hasObjectMember("graph"))
                master->graph(parsePluginGraph(root["master_track"]["graph"]));
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
