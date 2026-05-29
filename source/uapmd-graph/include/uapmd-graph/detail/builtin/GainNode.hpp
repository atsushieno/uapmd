#pragma once

#include <memory>

#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd::builtin {

    class GainNode : public AudioGraphNode {
    public:
        ~GainNode() override = default;

        virtual double gain() const = 0;
        virtual void gain(double value) = 0;
    };

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createGainNodeFactory();

}
