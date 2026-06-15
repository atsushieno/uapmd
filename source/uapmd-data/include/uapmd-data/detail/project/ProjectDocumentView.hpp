#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <uapmd/uapmd.hpp>
#include "ProjectDocumentEvents.hpp"
#include "../timeline/TimelineTypes.hpp"

namespace uapmd {

    struct ProjectTrackSnapshot {
        ProjectObjectId trackId;
        int32_t trackIndex{-1};
        bool masterTrack{false};
    };

    struct ProjectClipSnapshot {
        ProjectObjectId clipId;
        ProjectObjectId trackId;
        int32_t trackIndex{-1};
        int32_t clipNumericId{-1};
        int32_t sourceNodeId{-1};
        ClipType clipType{ClipType::Audio};
        std::string name;
        std::string filepath;
        TimelinePosition position;
        int64_t durationSamples{0};
        uint32_t tickResolution{0};
        double clipTempo{120.0};
        std::vector<ClipMarker> markers;
        std::vector<AudioWarpPoint> audioWarps;
    };

    struct ProjectAudioSourceSnapshot {
        ProjectObjectId audioSourceId;
        ProjectObjectId clipId;
        std::string filepath;
        int32_t sourceNodeId{-1};
        uint32_t channelCount{0};
        double sampleRate{0.0};
        int64_t frameCount{0};
    };

    class ProjectDocumentView {
    public:
        virtual ~ProjectDocumentView() = default;

        virtual ProjectRevision currentRevision() const = 0;

        virtual std::optional<ProjectObjectId> masterTrackId() const = 0;
        virtual std::vector<ProjectObjectId> trackIds() const = 0;
        virtual std::vector<ProjectObjectId> clipIds(ProjectObjectId trackId) const = 0;
        virtual std::vector<ProjectObjectId> audioSourceIds() const = 0;

        virtual std::optional<ProjectTrackSnapshot> getTrack(ProjectObjectId trackId) const = 0;
        virtual std::optional<ProjectClipSnapshot> getClip(ProjectObjectId clipId) const = 0;
        virtual std::optional<ProjectAudioSourceSnapshot> getAudioSource(ProjectObjectId audioSourceId) const = 0;

        virtual bool readClipUmpContent(
            ProjectObjectId clipId,
            std::vector<uapmd_ump_t>& events,
            std::vector<uint64_t>& timestampsInTicks,
            uint32_t& tickResolution) const = 0;

        virtual bool readAudioSourceSamples(
            ProjectObjectId audioSourceId,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const = 0;
    };

} // namespace uapmd
