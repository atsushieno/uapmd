
#include "UapmdMidiDevice.hpp"
#include "remidy-tooling/PluginScanTool.hpp"

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
                sequencer->instantiatePlugin(entry->format(), entry->pluginId(), [](int instanceId, std::string error) {
                    Logger::global()->logInfo("addSimpleTrack result: %d %s", instanceId, error.c_str());
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
        // FIXME: we need to design how we deal with multiple tracks.
        sequencer->enqueueUmp(0, ump, sizeInBytes, timestamp);
    }
}
