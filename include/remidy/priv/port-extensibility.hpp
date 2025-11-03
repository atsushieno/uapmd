#pragma once

// WIP WIP WIP

#include <string>
#include "../remidy.hpp"

namespace remidy {

    // We might have to reconsider how to deal with channel layout names:
    // - VST3 has no concept of named channel layouts; it only has SpeakerArrangement
    //   and there is no pre-defined combinations of speaker bits whereas (to our understanding)
    //   we need them, otherwise user has no idea on how those channels are in known order.
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

        uint32_t channels() const { return num_channels; }
        std::string& name() { return predefined_name; }

        friend bool operator==(const AudioChannelLayout &lhs, const AudioChannelLayout &rhs) {
            return lhs.predefined_name == rhs.predefined_name
                   && lhs.num_channels == rhs.num_channels;
        }

        static const AudioChannelLayout& mono() {
            static const AudioChannelLayout ret{"Mono", 1};
            return ret;
        }
        static const AudioChannelLayout& stereo() {
            static const AudioChannelLayout ret{"Stereo", 2};
            return ret;
        }
    };

    // Indicates the role of the audio bus.
    // `Main` means the primary bus, and `Aux` is for anything else e.g. sidechain.
    // Note that AudioUnit has no such concept and will assign Main at index 0.
    // On LV2 `isSideChain` port property is reflected.
    enum class AudioBusRole {
        Main,
        Aux
    };

    // Plugin defines them. Host just loads them. User has no control.
    class AudioBusDefinition {
        std::string bus_name{};
        AudioBusRole bus_role;
        std::vector<AudioChannelLayout> layouts;

    public:
        AudioBusDefinition(
            std::string busName,
            AudioBusRole role,
            std::vector<AudioChannelLayout> layouts = {}
        ) : bus_name(busName), bus_role(role), layouts(layouts) {
        }

        std::string name() { return bus_name; }

        AudioBusRole role() { return bus_role; }

        bool operator==(AudioBusDefinition & other) const {
            // assumes layouts are consistent where name and role are identical.
            return bus_name == other.bus_name && bus_role == other.bus_role;
        }

        const std::vector<AudioChannelLayout>& supportedChannelLayouts() { return layouts; }
    };

    // Host instantiates them per definition. User configures them.
    class AudioBusConfiguration {
        AudioBusDefinition def;
        bool is_enabled{true};
        AudioChannelLayout channel_layout{AudioChannelLayout::stereo()};
    public:
        AudioBusConfiguration(AudioBusDefinition& definition) : def(definition) {
        }

        bool enabled() { return is_enabled; }
        StatusCode enabled(bool newValue) {
            is_enabled = newValue;
            return StatusCode::OK;
        }

        AudioChannelLayout& channelLayout() { return channel_layout; }
        StatusCode channelLayout(const AudioChannelLayout& newValue) {
            if (auto& layouts = def.supportedChannelLayouts();
                !layouts.empty() && std::find(layouts.begin(), layouts.end(), newValue) == layouts.end())
                return StatusCode::UNSUPPORTED_CHANNEL_LAYOUT_REQUESTED;
            channel_layout = newValue;
            return StatusCode::OK;
        }
    };
}
