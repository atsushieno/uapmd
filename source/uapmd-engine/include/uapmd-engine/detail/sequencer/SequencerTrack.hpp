#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"

namespace uapmd {

    class SequencerTrack {
    protected:
        SequencerTrack() = default;

    public:
        virtual ~SequencerTrack() = default;
        static std::unique_ptr<SequencerTrack> create(
            const AudioGraphProviderRegistry& registry,
            size_t eventBufferSizeInBytes,
            const std::string& graphProviderId);

        virtual AudioPluginGraph& graph() = 0;
        virtual bool replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) = 0;
        virtual uint32_t latencyInSamples() = 0;
        virtual uint32_t renderLeadInSamples() = 0;
        virtual double tailLengthInSeconds() = 0;
        virtual std::vector<int32_t>& orderedInstanceIds() = 0;

        virtual bool bypassed() = 0;
        virtual bool frozen() = 0;
        virtual void bypassed(bool value) = 0;
        virtual void frozen(bool value) = 0;

        // UMP group assignment per plugin instance (0–15).
        // findAvailableGroup returns the lowest unused group, or 0xFF if all 16 are taken.
        virtual void    setInstanceGroup(int32_t instanceId, uint8_t group) = 0;
        virtual uint8_t getInstanceGroup(int32_t instanceId) const = 0; // 0xFF = unknown
        virtual uint8_t findAvailableGroup() const = 0;
    };

}
