#pragma once

#include <memory>

#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd::builtin {

    class AnalyserNode : public AudioGraphNode {
    public:
        ~AnalyserNode() override = default;

        virtual uint32_t frequencyBinCount() const = 0;
        virtual void getFloatFrequencyData(float* values, uint32_t valueCount) const = 0;
        virtual void getFloatTimeDomainData(float* values, uint32_t valueCount) const = 0;
        virtual void getMagnitudeData(float* values, uint32_t valueCount) const = 0;
        virtual void analyseInput(AudioProcessContext& process) = 0;
        virtual void analyseOutput(AudioProcessContext& process) = 0;
        virtual void reset() = 0;
    };

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createAnalyserNodeFactory();
    std::unique_ptr<AnalyserNode> createAnalyserNode(const AudioGraphNodeDescriptor& descriptor);

}
