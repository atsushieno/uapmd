#include <cstdint>
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

bool remidy::PluginInstanceAAP::PluginBusesAAP::hasEventInputs() {
    auto instance = owner->aapInstance();
    for (size_t i = 0, n = instance->getNumPorts(); i < n; i++) {
        auto port = instance->getPort(i);
        if (port->getContentType() == AAP_CONTENT_TYPE_MIDI2 && port->getPortDirection() == AAP_PORT_DIRECTION_INPUT)
            return true;
    }
    return false;
}

bool remidy::PluginInstanceAAP::PluginBusesAAP::hasEventOutputs() {
    auto instance = owner->aapInstance();
    for (size_t i = 0, n = instance->getNumPorts(); i < n; i++) {
        auto port = instance->getPort(i);
        if (port->getContentType() == AAP_CONTENT_TYPE_MIDI2 && port->getPortDirection() == AAP_PORT_DIRECTION_OUTPUT)
            return true;
    }
    return false;
}
