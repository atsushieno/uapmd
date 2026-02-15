
#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>

#include <midicci/midicci.hpp>
#include "uapmd/uapmd.hpp"

using namespace midicci::commonproperties;

namespace uapmd {

    UapmdFunctionBlock::UapmdFunctionBlock(std::shared_ptr<MidiIOFeature> midiDevice,
                                     AudioPluginNode* pluginNode,
                                     std::string deviceName,
                                     std::string manufacturerName,
                                     std::string versionString)
      : plugin_node(pluginNode),
        midi_device(std::move(midiDevice)) {
        uapmd_sessions = UapmdMidiCISession::create(this, pluginNode->instance(), deviceName, manufacturerName, versionString);
        if (midi_device)
            midi_device->addInputHandler(umpReceived, this);
    }

    UapmdFunctionBlock::~UapmdFunctionBlock() {
        if (midi_device)
            midi_device->removeInputHandler(umpReceived);
    }

    void UapmdFunctionBlock::initialize() {
        uapmd_sessions->setupMidiCISession();
    }

    void UapmdFunctionBlock::umpReceived(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        static_cast<UapmdFunctionBlock*>(context)->umpReceived(ump, sizeInBytes, timestamp);
    }

    void UapmdFunctionBlock::umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        if (!plugin_node)
            return;

        uapmd_sessions->interceptUmpInput(ump, sizeInBytes, timestamp);

        plugin_node->scheduleEvents(timestamp, ump, sizeInBytes);
    }

}
