#pragma once

#include <cstdint>
#include <memory>

#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd::builtin {

    // Mirrors Web Audio's ChannelMergerNode: numberOfInputs mono buses are combined
    // into a single output bus with numberOfInputs channels (output channel i = input
    // bus i, channel 0). In the DAG graph this is a genuine multi-bus merge. In the
    // linear graph, where every node shares one track-wide single-bus context, it
    // degrades to a channel passthrough clamped to numberOfInputs (there is no second
    // stream to merge with).
    class ChannelMergerNode : public AudioGraphNode {
    public:
        ~ChannelMergerNode() override = default;

        virtual uint32_t numberOfInputs() const = 0;
    };

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createChannelMergerNodeFactory();

}
