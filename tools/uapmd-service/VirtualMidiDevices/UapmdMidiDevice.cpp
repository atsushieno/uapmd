
#include "UapmdMidiDevice.hpp"
#include "cmidi2.h"

#include <memory>

namespace uapmd {

    UapmdMidiDevice::UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version) :
        deviceName(deviceName), manufacturer(manufacturer), version(version),
        // FIXME: do we need valid sampleRate here?
        audioPluginHost(new SequenceProcessor(44100, 1024, 4096, AudioPluginHostPAL::instance())) {
    }

    int32_t UapmdMidiDevice::channelToTrack(int32_t group, int32_t channel) {
        // FIXME: implement
        return 0;
    }

    uapmd_status_t UapmdMidiDevice::start() {
        platformDevice = std::make_unique<PlatformVirtualMidiDevice>(deviceName, manufacturer, version);

        platformDevice->addInputHandler(umpReceived, this);

        return 0;
    }

    uapmd_status_t UapmdMidiDevice::stop() {
        platformDevice->removeInputHandler(umpReceived);

        platformDevice.reset(nullptr);

        return 0;
    }

    void
    UapmdMidiDevice::umpReceived(void *context, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        static_cast<UapmdMidiDevice*>(context)->umpReceived(ump, sizeInBytes, timestamp);
    }

    void UapmdMidiDevice::umpReceived(uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        auto & tracks = audioPluginHost->tracks();
        CMIDI2_UMP_SEQUENCE_FOREACH(ump, sizeInBytes, iter) {
            auto u = (cmidi2_ump*) iter;
            auto group = cmidi2_ump_get_group(u);
            auto channel = cmidi2_ump_get_channel(u);

            auto messageType = cmidi2_ump_get_message_type(u);
            switch (messageType) {
                case CMIDI2_MESSAGE_TYPE_UTILITY:
                    switch (cmidi2_ump_get_status_code(u)) {
                        case CMIDI2_UTILITY_STATUS_JR_TIMESTAMP:
                            timestamp += static_cast<uapmd_timestamp_t >(1000000000.0 * cmidi2_ump_get_jr_timestamp_timestamp(u) / 31250);
                            break;
                        case CMIDI2_UTILITY_STATUS_DELTA_CLOCKSTAMP:
                            // FIXME: implement (we need DCTPQ stored somewhere)
                            break;
                    }
                case CMIDI2_MESSAGE_TYPE_UMP_STREAM:
                    // do we have to manage something here? mappings?
                    continue;
                default:
                    auto trackIndex = channelToTrack(group, channel);
                    if (tracks.size() <= trackIndex)
                        continue; // ignore out of range FIXME: should log an error?
                    auto track = tracks[trackIndex];
                    track->scheduleEvents(timestamp, u, cmidi2_ump_get_message_size_bytes(u));
                    break;
            }
        }
    }
}
