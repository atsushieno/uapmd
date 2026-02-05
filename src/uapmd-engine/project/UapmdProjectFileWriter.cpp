#include "UapmdProjectFile.hpp"
#include "UapmdProjectFileImpl.hpp"
#include <choc/text/choc_JSON.h>
#include <fstream>
#include <map>

namespace uapmd {
    // Helper to generate unique anchor IDs for tracks and clips
    class AnchorIdGenerator {
        std::map<UapmdClipDataReferencible*, std::string> id_map_;

    public:
        void registerTrack(UapmdProjectTrackData* track, size_t trackIndex, bool isMaster = false) {
            if (isMaster)
                id_map_[track] = "master_track";
            else
                id_map_[track] = "track_" + std::to_string(trackIndex);

            // Register clips within this track
            size_t clipIdx = 0;
            for (auto& clip : track->clips()) {
                std::string clipId = id_map_[track] + "_clip_" + std::to_string(clipIdx);
                id_map_[clip.get()] = clipId;
                ++clipIdx;
            }
        }

        std::string getId(UapmdClipDataReferencible* ref) {
            auto it = id_map_.find(ref);
            return it != id_map_.end() ? it->second : "";
        }
    };

    static choc::value::Value serializeClip(
        UapmdProjectClipData* clip,
        AnchorIdGenerator& idGen)
    {
        auto obj = choc::value::createObject("UapmdClip");

        auto pos = clip->position();

        // Serialize anchor reference
        if (pos.anchor != nullptr) {
            std::string anchorId = idGen.getId(pos.anchor);
            if (!anchorId.empty())
                obj.addMember("anchor", anchorId);
        }

        // Serialize position
        obj.addMember("position_samples", static_cast<int64_t>(pos.samples));

        // Serialize file reference
        auto filePath = clip->file();
        if (!filePath.empty())
            obj.addMember("file", filePath.string());

        // Serialize MIME type
        auto mime = clip->mimeType();
        if (!mime.empty())
            obj.addMember("mime_type", mime);

        return obj;
    }

    static choc::value::Value serializePluginGraph(UapmdProjectPluginGraphData* graph) {
        auto obj = choc::value::createObject("UapmdPluginGraph");

        auto extFile = graph->external_file();
        if (!extFile.empty())
            obj.addMember("external_file", extFile.string());

        auto plugins = graph->plugins();
        if (!plugins.empty()) {
            auto pluginsArray = choc::value::createEmptyArray();
            for (const auto& plugin : plugins) {
                auto pluginObj = choc::value::createObject("Plugin");
                pluginObj.addMember("plugin_id", plugin.plugin_id);
                pluginObj.addMember("format", plugin.format);
                pluginObj.addMember("state_file", plugin.state_file);
                pluginsArray.addArrayElement(pluginObj);
            }
            obj.addMember("plugins", pluginsArray);
        }

        return obj;
    }

    static choc::value::Value serializeTrack(
        UapmdProjectTrackData* track,
        AnchorIdGenerator& idGen)
    {
        auto obj = choc::value::createObject("UapmdTrack");

        // Serialize plugin graph
        if (track->graph())
            obj.addMember("graph", serializePluginGraph(track->graph()));

        // Serialize clips
        auto& clips = track->clips();
        if (!clips.empty()) {
            auto clipsArray = choc::value::createEmptyArray();
            for (auto& clip : clips)
                clipsArray.addArrayElement(serializeClip(clip.get(), idGen));
            obj.addMember("clips", clipsArray);
        }

        return obj;
    }

    bool UapmdProjectDataWriter::write(UapmdProjectData* data, std::filesystem::path file) {
        if (!data)
            return false;

        // Build anchor ID map
        AnchorIdGenerator idGen;
        size_t trackIndex = 0;
        for (auto* track : data->tracks()) {
            idGen.registerTrack(track, trackIndex);
            ++trackIndex;
        }
        if (data->masterTrack())
            idGen.registerTrack(data->masterTrack(), 0, true);

        // Create root JSON object
        auto root = choc::value::createObject("UapmdProject");

        // Serialize tracks
        auto& tracks = data->tracks();
        if (!tracks.empty()) {
            auto tracksArray = choc::value::createEmptyArray();
            for (auto* track : tracks)
                tracksArray.addArrayElement(serializeTrack(track, idGen));
            root.addMember("tracks", tracksArray);
        }

        // Serialize master track
        if (data->masterTrack())
            root.addMember("master_track", serializeTrack(data->masterTrack(), idGen));

        // Convert to JSON string
        std::string jsonStr = choc::json::toString(root, true);

        // Write to file
        std::ofstream ofs(file);
        if (!ofs)
            return false;

        ofs << jsonStr;
        return ofs.good();
    }
}
