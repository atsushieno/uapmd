#include <cstring>
#include <aap/ext/buses.h>
#include "PluginFormatAAP.hpp"

remidy::PluginInstanceAAP::PluginBusesAAP::PluginBusesAAP(PluginInstanceAAP* owner) : owner(owner) {
    inspectBuses();
}

void remidy::PluginInstanceAAP::PluginBusesAAP::inspectBuses() {
    audio_in_buses.clear();
    audio_out_buses.clear();
    bus_storage.clear();
    auto* instance = owner->aapInstance();
    if (!instance)
        return;

    for (auto direction : {AAP_PORT_DIRECTION_INPUT, AAP_PORT_DIRECTION_OUTPUT}) {
        auto& buses = direction == AAP_PORT_DIRECTION_INPUT ? audio_in_buses : audio_out_buses;
        for (int32_t i = 0, n = instance->getNumBuses(AAP_BUS_KIND_AUDIO, direction); i < n; i++) {
            auto* info = instance->getBus(AAP_BUS_KIND_AUDIO, direction, i);
            std::string layoutName = info->getLayout();
            if (layoutName == AAP_BUS_LAYOUT_MONO)
                layoutName = "Mono";
            else if (layoutName == AAP_BUS_LAYOUT_STEREO)
                layoutName = "Stereo";
            AudioChannelLayout layout{layoutName, static_cast<uint32_t>(info->getChannelCount())};
            AudioBusDefinition definition{info->getName(),
                info->getRole() == AAP_BUS_ROLE_MAIN ? AudioBusRole::Main : AudioBusRole::Aux, {layout}};
            auto bus = std::make_unique<AudioBusConfiguration>(definition);
            bus->channelLayout(layout);
            bus->enabled(info->isEnabled());
            buses.push_back(bus.get());
            bus_storage.push_back(std::move(bus));
        }
    }
}

void remidy::PluginInstanceAAP::PluginBusesAAP::configure(ConfigurationRequest& configuration) {
    auto* instance = dynamic_cast<aap::RemotePluginInstance*>(owner->aapInstance());
    if (instance && instance->isBusMode() &&
        instance->getNumBuses(AAP_BUS_KIND_AUDIO, AAP_PORT_DIRECTION_INPUT) +
        instance->getNumBuses(AAP_BUS_KIND_AUDIO, AAP_PORT_DIRECTION_OUTPUT) <= AAP_MAX_BUSES) {
        aap_bus_layout_request_t request{};
        bool changed = false;
        for (auto direction : {AAP_PORT_DIRECTION_INPUT, AAP_PORT_DIRECTION_OUTPUT}) {
            const auto& requested = direction == AAP_PORT_DIRECTION_INPUT
                ? configuration.mainInputChannels : configuration.mainOutputChannels;
            for (int32_t i = 0, n = instance->getNumBuses(AAP_BUS_KIND_AUDIO, direction); i < n; i++) {
                auto* info = instance->getBus(AAP_BUS_KIND_AUDIO, direction, i);
                auto& bus = request.buses[request.count++];
                bus.id = info->getId();
                bus.enabled = info->isEnabled();
                bus.channel_count = info->getChannelCount();
                std::string layout = info->getLayout();
                if (info->getRole() == AAP_BUS_ROLE_MAIN && requested.has_value()) {
                    bus.enabled = requested.value() > 0;
                    bus.channel_count = static_cast<int32_t>(requested.value());
                    if (bus.channel_count != info->getChannelCount())
                        layout = aap::BusInformation::getDefaultLayoutForChannelCount(bus.channel_count);
                    changed |= bus.enabled != info->isEnabled() || bus.channel_count != info->getChannelCount();
                }
                std::strncpy(bus.layout, layout.c_str(), sizeof(bus.layout) - 1);
            }
        }
        // Fixed-layout plugins may reject the request. Keep their actual layout in that case.
        if (changed) {
            auto error = instance->applyBusLayout(request);
            if (!error.empty())
                Logger::global()->logWarning("AAP bus layout request rejected: %s", error.c_str());
        }
    }
    inspectBuses();
    // A host can mute a main bus even when the plugin cannot disable its buffers.
    if (configuration.mainInputChannels == 0 && mainInputBusIndex() >= 0)
        audio_in_buses[mainInputBusIndex()]->enabled(false);
    if (configuration.mainOutputChannels == 0 && mainOutputBusIndex() >= 0)
        audio_out_buses[mainOutputBusIndex()]->enabled(false);
}

bool remidy::PluginInstanceAAP::PluginBusesAAP::hasEventInputs() {
    auto* instance = owner->aapInstance();
    return instance && instance->getNumBuses(AAP_BUS_KIND_EVENT, AAP_PORT_DIRECTION_INPUT) > 0;
}

bool remidy::PluginInstanceAAP::PluginBusesAAP::hasEventOutputs() {
    auto* instance = owner->aapInstance();
    return instance && instance->getNumBuses(AAP_BUS_KIND_EVENT, AAP_PORT_DIRECTION_OUTPUT) > 0;
}

int32_t remidy::PluginInstanceAAP::PluginBusesAAP::mainInputBusIndex() {
    for (size_t i = 0; i < audio_in_buses.size(); i++)
        if (audio_in_buses[i]->role() == AudioBusRole::Main)
            return static_cast<int32_t>(i);
    return -1;
}

int32_t remidy::PluginInstanceAAP::PluginBusesAAP::mainOutputBusIndex() {
    for (size_t i = 0; i < audio_out_buses.size(); i++)
        if (audio_out_buses[i]->role() == AudioBusRole::Main)
            return static_cast<int32_t>(i);
    return -1;
}
