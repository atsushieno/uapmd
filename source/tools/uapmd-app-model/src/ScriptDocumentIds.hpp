#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uapmd-app-model/uapmd-app-model.hpp>

// Stable document IDs used by the JS and MCP APIs, resolved on the main thread.
namespace uapmd_app::script_ids {

    inline uapmd::ProjectDocument& document() {
        return AppModel::instance().sequencer().engine()->timeline().document();
    }

    // Returns uapmd::kMasterTrackIndex for the master track.
    inline std::optional<int32_t> trackIndex(std::string_view trackId) {
        if (trackId.empty())
            return std::nullopt;
        auto track = document().view().getTrack(uapmd::ProjectObjectId{trackId});
        if (!track || (track->trackIndex < 0 && !track->masterTrack))
            return std::nullopt;
        return track->trackIndex;
    }

    inline std::string trackId(int32_t trackIndex) {
        return document().addresses().trackReferenceId(trackIndex).value_or(std::string{});
    }

    inline std::string masterTrackId() {
        return document().view().masterTrackId().value_or(std::string{});
    }

    inline std::optional<uapmd::ProjectClipSnapshot> clip(std::string_view clipId) {
        if (clipId.empty())
            return std::nullopt;
        return document().view().getClip(uapmd::ProjectObjectId{clipId});
    }

    inline std::vector<uapmd::ProjectClipSnapshot> clipsOnTrack(std::string_view trackId) {
        std::vector<uapmd::ProjectClipSnapshot> result;
        const auto& view = document().view();
        for (const auto& id : view.clipIds(uapmd::ProjectObjectId{trackId}))
            if (auto snapshot = view.getClip(id))
                result.push_back(std::move(*snapshot));
        return result;
    }

    inline std::string clipId(int32_t trackIndex, int32_t numericClipId) {
        for (const auto& snapshot : clipsOnTrack(trackId(trackIndex)))
            if (snapshot.clipNumericId == numericClipId)
                return snapshot.clipId;
        return {};
    }

    // Marker time references name their owner by its authored reference id.
    inline std::string markerOwnerReferenceId(const uapmd::ProjectClipSnapshot& clip, std::string_view masterReferenceId) {
        if (clip.trackIndex == uapmd::kMasterTrackIndex)
            return std::string{masterReferenceId};
        auto* track = document().addresses().timelineTrack(clip.trackId);
        auto* data = track ? track->clipManager().getClip(clip.clipNumericId) : nullptr;
        return data ? data->referenceId : std::string{};
    }
}
