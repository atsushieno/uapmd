#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

#include "AudioGraphExtension.hpp"

namespace uapmd_graph {

    enum class AudioPluginGraphEndpointType {
        GraphInput,
        Plugin,
        GraphOutput,
    };

    enum class AudioPluginGraphBusType {
        Audio,
        Event,
    };

    struct AudioPluginGraphEndpoint {
        AudioPluginGraphEndpointType type{AudioPluginGraphEndpointType::Plugin};
        std::string node_id{};
        int32_t instance_id{-1};
        uint32_t bus_index{0};
    };

    struct AudioPluginGraphConnection {
        int64_t id{0};
        AudioPluginGraphBusType bus_type{AudioPluginGraphBusType::Audio};
        AudioPluginGraphEndpoint source{};
        AudioPluginGraphEndpoint target{};
    };

    // Explicit edge editing between graph nodes. A graph that only supports a
    // linear chain does not offer this extension; one that lets the user wire
    // arbitrary edges does. Callers must reach it through
    // `AudioGraph::getExtension<GraphConnectionExtension>()` rather than by
    // casting to a concrete graph class, so that any provider -- including one
    // supplied by an addin -- can serve connection editing.
    //
    // None of these are audio-thread safe.
    class GraphConnectionExtension : public AudioGraphExtension {
    public:
        ~GraphConnectionExtension() override = default;

        virtual std::vector<AudioPluginGraphConnection> connections() = 0;
        virtual uapmd_status_t connect(const AudioPluginGraphConnection& connection) = 0;
        virtual bool disconnect(int64_t connectionId) = 0;
        virtual void clearConnections() = 0;
    };

} // namespace uapmd_graph
