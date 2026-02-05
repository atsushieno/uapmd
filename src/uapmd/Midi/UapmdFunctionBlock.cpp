
#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>

#include "uapmd/uapmd.hpp"

using namespace midicci::commonproperties;

namespace uapmd {

    UapmdFunctionBlock::UapmdFunctionBlock(std::shared_ptr<MidiIOFeature> midiDevice,
                                     SequencerFeature* sharedSequencer,
                                     int32_t instanceId,
                                     std::string deviceName,
                                     std::string manufacturerName,
                                     std::string versionString)
        : sequencer(sharedSequencer),
          instance_id(instanceId),
          midi_device(std::move(midiDevice)) {
        uapmd_sessions = std::make_unique<UapmdMidiCISession>(this, sharedSequencer, deviceName, manufacturerName, versionString);
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
        if (!sequencer)
            return;

        uapmd_sessions->interceptUmpInput(ump, sizeInBytes, timestamp);

        if (instance_id >= 0) {
            sequencer->enqueueUmp(instance_id, ump, sizeInBytes, timestamp);
            return;
        }

        sequencer->enqueueUmp(instance_id, ump, sizeInBytes, timestamp);
    }

}
