#pragma once

#include <algorithm>
#include <vector>
#include <uapmd/uapmd.hpp>
#include "UapmdProjectFile.hpp"

namespace uapmd {
    enum class UapmdClipMoveResult {
        Success,
        Rejected,
        Failed
    };

    class UapmdProjectClip {
        uapmd_timestamp_t time_position{};

    public:
        uapmd_timestamp_t timePosition() { return time_position; }

        UapmdClipMoveResult move(uapmd_timestamp_t newTimePosition);
    };

    class UapmdAudioClip : public UapmdProjectClip {
        std::unique_ptr<AudioFileReader> reader{};

    public:
        explicit UapmdAudioClip(std::unique_ptr<AudioFileReader>&& reader)
            : reader(std::move(reader)) {
        }
    };

    class UapmdMidi2Clip : public UapmdProjectClip {
        std::vector<uapmd_ump_t> data_{};
    public:

        std::vector<uapmd_ump_t>& data() { return data_; }
    };

    class UapmdSequenceTrack {
    public:
        virtual ~UapmdSequenceTrack() = default;

        virtual AudioPluginGraph* graph();

        virtual std::vector<std::unique_ptr<UapmdProjectClipData>>& clips();
    };

    class UapmdSequence {
        std::vector<std::unique_ptr<UapmdSequenceTrack>> tracks_{};

    public:

        std::vector<UapmdSequenceTrack*> tracks() {
            std::vector<UapmdSequenceTrack*> ret{};
            for (auto& track : tracks_)
                ret.push_back(track.get());
            return ret;
        }
    };
}