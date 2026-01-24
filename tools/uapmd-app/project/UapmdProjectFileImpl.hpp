#pragma once

#include "UapmdProjectFile.hpp"
#include <memory>
#include <string>
#include <vector>

namespace uapmd {
    // Concrete implementation of UapmdProjectClipData
    class UapmdProjectClipDataImpl : public UapmdProjectClipData {
        UapmdTimelinePosition position_{};
        std::filesystem::path file_{};
        std::string mime_type_{};

    public:
        UapmdProjectClipDataImpl() = default;

        UapmdTimelinePosition position() override { return position_; }
        std::filesystem::path& file() override { return file_; }
        std::string mimeType() override { return mime_type_; }

        void setPosition(UapmdTimelinePosition pos) { position_ = pos; }
        void setFile(const std::filesystem::path& f) { file_ = f; }
        void setMimeType(const std::string& mime) { mime_type_ = mime; }
    };

    // Concrete implementation of UapmdProjectPluginGraphData
    class UapmdProjectPluginGraphDataImpl : public UapmdProjectPluginGraphData {
        std::filesystem::path external_file_{};
        std::vector<UapmdProjectPluginNodeData> plugins_{};

    public:
        UapmdProjectPluginGraphDataImpl() = default;

        std::filesystem::path external_file() override { return external_file_; }
        std::vector<UapmdProjectPluginNodeData> plugins() override { return plugins_; }

        void setExternalFile(const std::filesystem::path& f) { external_file_ = f; }
        void setPlugins(std::vector<UapmdProjectPluginNodeData>&& p) { plugins_ = std::move(p); }
        void addPlugin(const UapmdProjectPluginNodeData& plugin) { plugins_.push_back(plugin); }
    };

    // Concrete implementation of UapmdProjectTrackData
    class UapmdProjectTrackDataImpl : public UapmdProjectTrackData {
        std::unique_ptr<UapmdProjectPluginGraphData> graph_{};
        std::vector<std::unique_ptr<UapmdProjectClipData>> clips_{};

    public:
        UapmdProjectTrackDataImpl()
            : graph_(std::make_unique<UapmdProjectPluginGraphDataImpl>()) {
        }

        UapmdProjectPluginGraphData* graph() override { return graph_.get(); }
        std::vector<std::unique_ptr<UapmdProjectClipData>>& clips() override { return clips_; }

        void setGraph(std::unique_ptr<UapmdProjectPluginGraphData>&& g) { graph_ = std::move(g); }
        void addClip(std::unique_ptr<UapmdProjectClipData>&& clip) {
            clips_.push_back(std::move(clip));
        }
    };

    // Concrete implementation of UapmdProjectData
    class UapmdProjectDataImpl : public UapmdProjectData {
        std::vector<std::unique_ptr<UapmdProjectTrackData>> tracks_{};
        std::unique_ptr<UapmdProjectTrackData> master_track_{};

    public:
        UapmdProjectDataImpl()
            : master_track_(std::make_unique<UapmdProjectTrackDataImpl>()) {
        }

        size_t addTrack(std::unique_ptr<UapmdProjectTrackData> newTrack) override {
            tracks_.push_back(std::move(newTrack));
            return tracks_.size() - 1;
        }

        bool removeTrack(size_t trackIndex) override {
            if (trackIndex >= tracks_.size())
                return false;
            tracks_.erase(tracks_.begin() + trackIndex);
            return true;
        }

        std::vector<UapmdProjectTrackData*>& tracks() override {
            // Note: This requires storing a separate vector of raw pointers
            // which is not ideal. Consider refactoring the API.
            static std::vector<UapmdProjectTrackData*> track_ptrs;
            track_ptrs.clear();
            for (auto& track : tracks_)
                track_ptrs.push_back(track.get());
            return track_ptrs;
        }

        UapmdProjectTrackData* masterTrack() override { return master_track_.get(); }

        // Direct access for internal use
        std::vector<std::unique_ptr<UapmdProjectTrackData>>& tracksOwned() { return tracks_; }
    };
}
