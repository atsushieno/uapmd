
#include "UapmdMidiDevice.hpp"
#include <iostream>

using namespace midicci::commonproperties;

#include <memory>

namespace uapmd {
    UapmdMidiDevice::UapmdMidiDevice(std::string& apiName, std::string& deviceName, std::string& manufacturer, std::string& version) :
        api_name(apiName), device_name(deviceName), manufacturer(manufacturer), version(version),
        // FIXME: do we need valid sampleRate here?
        sequencer(new AudioPluginSequencer(4096, 1024, 44100)) {
    }
    
    void UapmdMidiDevice::addPluginTrack(std::string& pluginName, std::string& formatName) {
        remidy_tooling::PluginScanTool scanner{};
        scanner.performPluginScanning();
        for(auto & entry : scanner.catalog.getPlugins()) {
            if ((pluginName.empty() || entry->displayName().contains(pluginName)) &&
                (formatName.empty() || entry->format() == formatName)) {
                Logger::global()->logInfo("Found %s", entry->bundlePath().c_str());
                sequencer->instantiatePlugin(entry->format(), entry->pluginId(), [&](int instanceId, std::string error) {
                    Logger::global()->logInfo("addSimpleTrack result: %d %s", instanceId, error.c_str());

                    midicci::MidiCIDeviceConfiguration ci_config{
                            midicci::DEFAULT_RECEIVABLE_MAX_SYSEX_SIZE,
                            midicci::DEFAULT_MAX_PROPERTY_CHUNK_SIZE,
                            std::format("uapmd-service {}", instanceId),
                            0
                    };
                    uint32_t muid{static_cast<uint32_t>(rand() & 0x7F7F7F7F)};

                    // UmpReceived() -> ci_input_forwarders ->
                    auto input_listener_adder = [&](midicci::musicdevice::MidiInputCallback callback) {
                        ci_input_forwarders.push_back(std::move(callback));
                    };
                    auto sender = [&](const uint8_t* data, size_t offset, size_t length, uint64_t timestamp) {
                        platformDevice->send((uapmd_ump_t*) (data + offset), length, (uapmd_timestamp_t) timestamp);
                    };
                    midicci::musicdevice::MidiCISessionSource source{midicci::musicdevice::MidiTransportProtocol::UMP, input_listener_adder, sender};
                    auto ciSession = create_midi_ci_session(source, muid, std::move(ci_config), [&](std::string msg, bool outgoing) {
                        std::cerr << "[UAPMD LOG " << (outgoing ? "OUT] " : "In] ") << msg << std::endl;
                    });
                    auto& ciDevice = ciSession->get_device();

                    // configure parameter list
                    ciDevice.get_property_host_facade().addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::allCtrlListMetadata));
                    ciDevice.get_property_host_facade().addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::chCtrlListMetadata));
                    ciDevice.get_property_host_facade().addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::programListMetadata));
                    ciDevice.get_property_host_facade().addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::stateListMetadata));
                    ciDevice.get_property_host_facade().addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::stateMetadata));
                    std::vector<commonproperties::MidiCIControl> allCtrlList{};
                    auto parameterList = sequencer->getParameterList(instanceId);
                    for (auto& p : parameterList) {
                        commonproperties::MidiCIControl ctrl{p.name, MidiCIControlType::NRPN, "",
                                                             std::vector<uint8_t>{static_cast<unsigned char>((uint8_t) p.index / 0x80), static_cast<unsigned char>((uint8_t) p.index % 0x80)}
                        };
                        allCtrlList.push_back(ctrl);
                    }
                    StandardPropertiesExtensions::setAllCtrlList(ciDevice, allCtrlList);
                    ci_sessions[muid] = std::move(ciSession);
                });
                return;
            }
        }
        Logger::global()->logError("Plugin %s in format %s not found", pluginName.c_str(), formatName.c_str());
    }

    uapmd_status_t UapmdMidiDevice::start() {
        platformDevice = std::make_unique<PlatformVirtualMidiDevice>(api_name, device_name, manufacturer, version);

        platformDevice->addInputHandler(umpReceived, this);

        sequencer->startAudio();

        return 0;
    }

    uapmd_status_t UapmdMidiDevice::stop() {
        sequencer->stopAudio();

        platformDevice->removeInputHandler(umpReceived);

        platformDevice.reset(nullptr);

        return 0;
    }

    void
    UapmdMidiDevice::umpReceived(void *context, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        static_cast<UapmdMidiDevice*>(context)->umpReceived(ump, sizeInBytes, timestamp);
    }

    void UapmdMidiDevice::umpReceived(uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {

        for (auto& fw : ci_input_forwarders)
            fw((uint8_t*) (void*) ump, 0, sizeInBytes, timestamp);

        // FIXME: we need to design how we deal with multiple tracks.
        sequencer->enqueueUmp(0, ump, sizeInBytes, timestamp);
    }
}
