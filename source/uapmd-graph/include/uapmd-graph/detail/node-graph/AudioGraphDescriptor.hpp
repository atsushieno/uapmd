#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace uapmd {

    using AudioGraphScalarValue = std::variant<bool, int64_t, double, std::string>;

    struct AudioGraphPluginPayload {
        std::string format{};
        std::string plugin_id{};
        std::string state_file{};
    };

    struct AudioGraphNodeDescriptor {
        std::string node_id{};
        std::string node_type{};
        std::string display_name{};
        std::unordered_map<std::string, AudioGraphScalarValue> options{};
        std::unordered_map<std::string, AudioGraphScalarValue> parameters{};
        std::unordered_map<std::string, AudioGraphScalarValue> metadata{};
        std::optional<AudioGraphPluginPayload> plugin{};
    };

    struct AudioGraphEndpointDescriptor {
        std::string node_id{};
        std::string port{};
        std::optional<uint32_t> channel{};
    };

    struct AudioGraphConnectionDescriptor {
        std::string connection_id{};
        std::string kind{"audio"};
        AudioGraphEndpointDescriptor source{};
        AudioGraphEndpointDescriptor target{};
    };

    struct AudioGraphDescriptor {
        uint32_t schema_version{1};
        std::string graph_type{};
        std::vector<AudioGraphNodeDescriptor> nodes{};
        std::vector<AudioGraphConnectionDescriptor> connections{};
    };

}
