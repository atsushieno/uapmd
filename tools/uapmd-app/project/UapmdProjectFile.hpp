#pragma once

#include <algorithm>
#include <vector>
#include <filesystem>

namespace uapmd {
    class UapmdClipDataReferencible {
    public:
        virtual ~UapmdClipDataReferencible() = default;

        virtual uint64_t absolutePositionInSamples() = 0;
    };

    enum class UapmdAnchorOrigin {
        Start,
        End
    };

    struct UapmdTimelinePosition {
        // can be used to calculate the absolute position.
        UapmdClipDataReferencible* anchor{nullptr};
        UapmdAnchorOrigin origin{UapmdAnchorOrigin::Start};
        uint64_t samples{};
    };

    class UapmdProjectClipData : public UapmdClipDataReferencible {
    public:
        virtual UapmdTimelinePosition position() = 0;
        virtual std::filesystem::path& file() = 0;
        // The returned value can be empty, then guessed from the filename.
        // (`.midi2` has no registered MIME type so far...)
        virtual std::string mimeType() = 0;

        uint64_t absolutePositionInSamples() override {
            auto pos = position();
            if (pos.anchor == nullptr)
                return pos.samples;
            return pos.anchor->absolutePositionInSamples() + pos.samples;
        }
    };

    struct UapmdProjectPluginNodeData {
        std::string plugin_id{};
        std::string format{};
        std::string state_file{};
    };

    class UapmdProjectPluginGraphData {
    public:
        virtual std::filesystem::path external_file() = 0;

        // `external_file` might indicate more complicated graph.
        // For example, we could implement extension support for `.filtergraph` file from JUCE AudioPluginHost.
        // If empty, then it's a simple linear list.
        virtual std::vector<UapmdProjectPluginNodeData> plugins() = 0;
    };

    class UapmdProjectTrackData : public UapmdClipDataReferencible {
    public:
        uint64_t absolutePositionInSamples() { return 0; }

        virtual UapmdProjectPluginGraphData* graph() = 0;

        virtual std::vector<std::unique_ptr<UapmdProjectClipData>>& clips() = 0;
    };

    class UapmdProjectData {
    public:
        virtual size_t addTrack(std::unique_ptr<UapmdProjectTrackData> newTrack) = 0;
        virtual bool removeTrack(size_t trackIndex) = 0;
        virtual std::vector<UapmdProjectTrackData*>& tracks() = 0;
        virtual UapmdProjectTrackData* masterTrack() = 0;
    };

    class UapmdProjectDataWriter {
    public:
        static bool write(UapmdProjectData* data, std::filesystem::path file);
    };

    class UapmdProjectDataReader {
    public:
        static std::unique_ptr<UapmdProjectData> read(std::filesystem::path file);
    };
}
