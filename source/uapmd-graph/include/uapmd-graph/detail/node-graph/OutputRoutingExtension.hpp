#pragma once

#include <cstdint>
#include <vector>

#include "AudioGraphExtension.hpp"

namespace uapmd {

    enum class TrackOutputRoutingTargetType {
        DISABLED = 0,
        MASTER_INPUT_BUS = 1,
        MAIN_MIX_BUS = 2,
    };

    struct TrackOutputRoutingTarget {
        TrackOutputRoutingTargetType type{TrackOutputRoutingTargetType::DISABLED};
        uint32_t bus_index{0};
        bool folded{false};
    };

    struct TrackOutputRoutingRule {
        uint32_t source_bus_index{0};
        TrackOutputRoutingTarget target{};
    };

    class OutputRoutingExtension : public AudioGraphExtension {
    public:
        ~OutputRoutingExtension() override = default;

        virtual std::vector<TrackOutputRoutingRule> outputRoutingRules() const = 0;
        virtual void outputRoutingRules(const std::vector<TrackOutputRoutingRule>& rules) = 0;
        virtual TrackOutputRoutingTarget outputRoutingTarget(uint32_t outputBusIndex) const = 0;
    };

} // namespace uapmd
