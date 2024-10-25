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

        static const AudioChannelLayout* mono() {
            static const AudioChannelLayout ret{"Mono", 1};
            return &ret;
        }
        static const AudioChannelLayout* stereo() {
            static const AudioChannelLayout ret{"Stereo", 2};
            return &ret;
        }
    };

    enum class AudioBusRole {
        Main,
        Aux
    };

    // Plugin defines them. Host just loads them. User has no control.
    class AudioBusDefinition {
        std::string bus_name{};
        AudioBusRole bus_role;
        std::vector<AudioChannelLayout*> layouts;

    public:
        AudioBusDefinition(std::string busName, AudioBusRole role) :
            bus_name(busName), bus_role(role) {
        }

        std::string name() { return bus_name; }

        AudioBusRole role() { return bus_role; }

        bool operator==(AudioBusDefinition & other) const {
            // assumes layouts are consistent where name and role are identical.
            return bus_name == other.bus_name && bus_role == other.bus_role;
        }

        std::vector<AudioChannelLayout*>& supportedChannelLayouts() { return layouts; }
    };

    // Host instantiates them per definition. User configures them.
    class AudioBusConfiguration {
        AudioBusDefinition* def;
        const AudioChannelLayout* channel_layout{AudioChannelLayout::stereo()};
    public:
        AudioBusConfiguration(AudioBusDefinition* definition) : def(definition) {
        }

        const AudioChannelLayout* channelLayout() { return channel_layout; }
        StatusCode channelLayout(const AudioChannelLayout* newValue) {
            if (auto layouts = def->supportedChannelLayouts();
                std::find(layouts.begin(), layouts.end(), newValue) == layouts.end())
            return StatusCode::UNSUPPORTED_CHANNEL_LAYOUT_REQUESTED;
            channel_layout = newValue;
            return StatusCode::OK;
        }
    };
}