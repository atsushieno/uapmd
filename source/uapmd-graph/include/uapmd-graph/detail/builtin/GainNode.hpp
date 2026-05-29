#pragma once

#include <memory>

#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd::builtin {

    class GainNode : public AudioGraphNode {
    public:
        ~GainNode() override = default;

        virtual double gain() const = 0;
        virtual void gain(double value) = 0;

        // Apply the gain ramp to the existing output buffers of `process` without
        // touching the input buffers.  Used when the GainNode lives outside a graph.
        virtual void applyToOutputs(AudioProcessContext& process) = 0;
    };

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createGainNodeFactory();
    // Creates a standalone GainNode (not via the graph's factory registry).
    std::unique_ptr<GainNode> createGainNode(const AudioGraphNodeDescriptor& descriptor);

}
