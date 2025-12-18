#include "VirtualMidiDeviceController.hpp"

#include <algorithm>
#include <future>

#include "../VirtualMidiDevices/LibreMidiVirtualMidiDevice.hpp"

namespace uapmd {

    VirtualMidiDeviceController::VirtualMidiDeviceController(AudioPluginSequencer* sharedSequencer)
        : sequencer_(sharedSequencer) {
    }

    int32_t VirtualMidiDeviceController::instantiatePluginOnTrack(int32_t trackIndex,
                                                                  std::string format,
                                                                  std::string pluginId,
                                                                  std::string& error) {
        auto promisePtr = std::make_shared<std::promise<std::pair<int32_t, std::string>>>();
        auto future = promisePtr->get_future();

        if (trackIndex < 0) {
            sequencer_->addSimplePluginTrack(format, pluginId,
                [promisePtr](int32_t instanceId, std::string err) {
                    promisePtr->set_value({instanceId, std::move(err)});
                });
        } else {
            sequencer_->addPluginToTrack(trackIndex, format, pluginId,
                [promisePtr](int32_t instanceId, std::string err) {
                    promisePtr->set_value({instanceId, std::move(err)});
                });
        }

        auto result = future.get();
        error = std::move(result.second);
        return result.first;
    }

    void VirtualMidiDeviceController::syncDeviceAssignments() {
        if (!sequencer_)
            return;
        for (auto& device : devices_) {
            if (!device)
                continue;
            if (auto group = sequencer_->pluginGroup(device->instanceId()); group.has_value()) {
                device->group(group.value());
            }
        }
    }

    std::shared_ptr<UapmdMidiDevice> VirtualMidiDeviceController::createDevice(const std::string& apiName,
                                                                               const std::string& deviceName,
                                                                               const std::string& manufacturer,
                                                                               const std::string& version,
                                                                               int32_t trackIndex,
                                                                               const std::string& formatName,
                                                                               const std::string& pluginId,
                                                                               std::string& errorMessage) {
        if (!sequencer_) {
            errorMessage = "Sequencer not available";
            return nullptr;
        }

        auto format = formatName;
        auto plugin = pluginId;
        auto instanceId = instantiatePluginOnTrack(trackIndex, std::move(format), std::move(plugin), errorMessage);
        if (!errorMessage.empty() || instanceId < 0) {
            return nullptr;
        }

        auto actualTrackIndex = sequencer_->findTrackIndexForInstance(instanceId);
        auto platformDevice = std::make_unique<LibreMidiVirtualMidiDevice>(apiName, deviceName, manufacturer, version);
        auto device = std::make_shared<UapmdMidiDevice>(std::move(platformDevice),
                                                        sequencer_,
                                                        instanceId,
                                                        actualTrackIndex,
                                                        apiName,
                                                        deviceName,
                                                        manufacturer,
                                                        version);

        if (auto group = sequencer_->pluginGroup(instanceId); group.has_value()) {
            device->group(group.value());
        }

        device->initialize();
        devices_.push_back(device);
        syncDeviceAssignments();

        if (!audio_running_) {
            sequencer_->startAudio();
            audio_running_ = true;
        }

        return device;
    }

    bool VirtualMidiDeviceController::removeDevice(int32_t instanceId) {
        auto it = std::find_if(devices_.begin(), devices_.end(), [instanceId](const std::shared_ptr<UapmdMidiDevice>& dev) {
            return dev && dev->instanceId() == instanceId;
        });
        if (it == devices_.end())
            return false;

        auto device = *it;
        if (device)
            device->stop();

        sequencer_->removePluginInstance(instanceId);
        devices_.erase(it);
        syncDeviceAssignments();

        if (devices_.empty() && audio_running_) {
            sequencer_->stopAudio();
            audio_running_ = false;
        }

        return true;
    }

}
