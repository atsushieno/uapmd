#include "../../../include/uapmd/priv/midi/UapmdMidiDevice.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>

#include "UapmdMidiCISessions.hpp"

using namespace midicci::commonproperties;

namespace uapmd {

    UapmdMidiDevice::UapmdMidiDevice(std::unique_ptr<PlatformVirtualMidiDevice> platformMidiDevice,
                                     AudioPluginSequencer* sharedSequencer,
                                     int32_t instanceId,
                                     int32_t trackIndex,
                                     std::string apiName,
                                     std::string deviceName,
                                     std::string manufacturerName,
                                     std::string versionString)
        : api_name(std::move(apiName)),
          sequencer(sharedSequencer),
          instance_id(instanceId),
          track_index(trackIndex),
          platform_device(std::move(platformMidiDevice)) {
        uapmd_sessions = std::make_unique<UapmdMidiCISessions>(this, deviceName, manufacturerName, versionString);
        platform_device->addInputHandler(&UapmdMidiDevice::umpReceivedTrampoline, this);
    }

    UapmdMidiDevice::~UapmdMidiDevice() {
        stop();
        teardownOutputHandler();
        platform_device->removeInputHandler(&UapmdMidiDevice::umpReceivedTrampoline);
    }

    void UapmdMidiDevice::teardownOutputHandler() {
        if (!sequencer || instance_id < 0)
            return;
        if (output_handler_registered) {
            sequencer->setPluginOutputHandler(instance_id, nullptr);
            output_handler_registered = false;
        }
    }

    void UapmdMidiDevice::setupMidiCISession() {
        uapmd_sessions->setupMidiCISession();
    }

    void UapmdMidiDevice::initialize() {
        if (sequencer) {
            if (auto groupOpt = sequencer->pluginGroup(instance_id); groupOpt.has_value()) {
                ump_group = groupOpt.value();
            }
        }
        setupMidiCISession();
    }

    uapmd_status_t UapmdMidiDevice::start() {
        if (!sequencer)
            return -1;

        return 0;
    }

    uapmd_status_t UapmdMidiDevice::stop() {
        return 0;
    }

    void UapmdMidiDevice::umpReceivedTrampoline(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        static_cast<UapmdMidiDevice*>(context)->umpReceived(ump, sizeInBytes, timestamp);
    }

    void UapmdMidiDevice::umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        if (!sequencer)
            return;

        uapmd_sessions->interceptUmpInput(ump, sizeInBytes, timestamp);

        if (instance_id >= 0) {
            sequencer->enqueueUmpForInstance(instance_id, ump, sizeInBytes, timestamp);
            return;
        }

        auto groupId = static_cast<uint8_t>((ump[0] >> 28) & 0x0F);
        if (auto instance = sequencer->instanceForGroup(groupId); instance.has_value()) {
            sequencer->enqueueUmpForInstance(instance.value(), ump, sizeInBytes, timestamp);
        } else {
            sequencer->enqueueUmp(groupId, ump, sizeInBytes, timestamp);
        }
    }

}
