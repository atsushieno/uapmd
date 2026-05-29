#pragma once

#include <memory>
#include <vector>

#include "AudioPluginGraph.hpp"

namespace uapmd {

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
        int32_t instance_id{-1};
        uint32_t bus_index{0};
    };

    struct AudioPluginGraphConnection {
        int64_t id{0};
        AudioPluginGraphBusType bus_type{AudioPluginGraphBusType::Audio};
        AudioPluginGraphEndpoint source{};
        AudioPluginGraphEndpoint target{};
    };

    class AudioPluginFullDAGraph : public AudioPluginGraph {
    protected:
        explicit AudioPluginFullDAGraph(std::string providerId)
            : AudioPluginGraph(std::move(providerId)) {}

    public:
        ~AudioPluginFullDAGraph() override = default;

        virtual std::vector<AudioPluginGraphConnection> connections() = 0;
        virtual uapmd_status_t connect(const AudioPluginGraphConnection& connection) = 0;
        virtual bool disconnect(int64_t connectionId) = 0;
        virtual void clearConnections() = 0;

        static std::unique_ptr<AudioPluginFullDAGraph> create(size_t eventBufferSizeInBytes);

    };

}
