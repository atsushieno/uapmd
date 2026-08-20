#pragma once

#include <memory>
#include <string>

#include "AudioPluginGraph.hpp"
#include "GraphConnectionExtension.hpp"

namespace uapmd_graph {

    // The built-in graph that supports arbitrary DAG topology. Nothing outside
    // this module should need to name it: reach its edge editing through
    // `AudioGraph::getExtension<GraphConnectionExtension>()`.
    class AudioPluginFullDAGraph : public AudioPluginGraph, public GraphConnectionExtension {
    protected:
        explicit AudioPluginFullDAGraph(std::string providerId)
            : AudioPluginGraph(std::move(providerId)) {}

    public:
        ~AudioPluginFullDAGraph() override = default;

        // `providerId` is stamped onto the graph so that whoever created it can
        // be identified again later without guessing from the graph's shape.
        static std::unique_ptr<AudioPluginFullDAGraph> create(size_t eventBufferSizeInBytes, std::string providerId = {});
    };

}
