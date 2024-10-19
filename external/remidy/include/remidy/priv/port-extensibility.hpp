#pragma once

// WIP WIP WIP

#include <string>
#include "../remidy.hpp"

namespace remidy {

    class AudioChannelLayout {
        std::string predefined_name;
        uint32_t num_channels;
    public:
        AudioChannelLayout(
            std::string name,
            uint32_t channels
        ) : predefined_name(name),
            num_channels(channels) {
        }

        uint32_t channels() { return num_channels; }
        std::string& name() { return predefined_name; }

        static AudioChannelLayout& mono() {
            static AudioChannelLayout ret{"Mono", 1};
            return ret;
        };
        static AudioChannelLayout& stereo() {
            static AudioChannelLayout ret{"Stereo", 2};
            return ret;
        }
    };

    enum class AudioBusRole {
        Main,
        Aux
    };

    // Plugin defines them. Host just loads them. User has no access.
    class AudioBusDefinition {
        std::string bus_name{};
        AudioBusRole bus_role;

    public:
        AudioBusDefinition(std::string busName, AudioBusRole role) :
            bus_name(busName), bus_role(role) {
        }

        std::string name() { return bus_name; }

        AudioBusRole role() { return bus_role; }

        bool operator==(AudioBusDefinition & other) const {
            return bus_name == other.bus_name && bus_role == other.bus_role;
        }
    };

    class AudioBusConfiguration {
        AudioBusDefinition* def;
        AudioChannelLayout& channel_layout{AudioChannelLayout::stereo()};
    public:
        AudioBusConfiguration(AudioBusDefinition* definition) : def(definition) {

        }

        AudioChannelLayout& channel() { return channel_layout; }
        StatusCode channel(AudioChannelLayout& newValue) {
            channel_layout = newValue;
            return StatusCode::OK;
        }
    };
}