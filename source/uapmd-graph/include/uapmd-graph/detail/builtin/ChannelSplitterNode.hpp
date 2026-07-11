#pragma once

#include <cstdint>
#include <memory>

#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd::builtin {

    // Mirrors Web Audio's ChannelSplitterNode: a single input bus is split into
    // numberOfOutputs mono output buses (output bus i, channel 0 = input channel i;
    // silent if the input has fewer channels than i). In the DAG graph this is a
    // genuine multi-bus split. In the linear graph, where every node shares one
    // track-wide single-bus context, it degrades to a channel passthrough clamped
    // to numberOfOutputs (there is nowhere to route separate outputs to).
    class ChannelSplitterNode : public AudioGraphNode {
    public:
        ~ChannelSplitterNode() override = default;

        virtual uint32_t numberOfOutputs() const = 0;
    };

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createChannelSplitterNodeFactory();

}
