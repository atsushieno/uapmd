#pragma once
#include "uapmd/uapmd.hpp"

namespace uapmd {
    // It is a "MIDI 2.0 Device" which contains zero or more Function Blocks indicated by a span of groups.
    // A UapmdFunctionBlock corresponds to a plugin instance.
    class UapmdFunctionDevice {
        MidiIOManagerFeature* midi_io_manager;
        std::map<int32_t,std::shared_ptr<UapmdFunctionBlock>> blocks{};

    public:
        explicit UapmdFunctionDevice(MidiIOManagerFeature* midiIoManager) : midi_io_manager(midiIoManager) {}

        std::vector<UapmdFunctionBlock*> devices() {
            std::vector<UapmdFunctionBlock*> result{};
            for (auto &val: blocks | std::views::values)
                if (val)  // Skip null shared_ptrs
                    result.push_back(val.get());
            return result;
        }

        bool createFunctionBlock(const std::string& apiName,
                        AudioPluginNode* pluginNode,
                        int32_t instanceId,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version) {
            uint8_t group = 0;
            while (group < 16) {
                bool conflict = false;
                for (const auto& block : blocks | std::views::values)
                    if (block && block->group() == group) {
                        conflict = true;
                        group++;
                    }
                if (!conflict)
                    break;
            }
            if (group >= 16) // no room anymore
                return false;
            std::shared_ptr<MidiIOFeature> midiDevice  = midi_io_manager->createMidiIOFeature(apiName, deviceName, manufacturer, version);
            if (!midiDevice) return false;

            const auto fb = std::make_shared<UapmdFunctionBlock>(midiDevice, pluginNode, deviceName, manufacturer, version);
            fb->group(group);
            blocks[instanceId] = fb;
            return true;
        }

        // FIXME: we should not really return shared_ptr here...
        std::shared_ptr<UapmdFunctionBlock> getDeviceByInstanceId(int32_t instanceId) {
            const auto it = blocks.find(instanceId);
            return it != blocks.end() ? it->second : nullptr;
        }

        void destroyDevice(const int32_t instanceId) {
            blocks.erase(instanceId);
        }

        bool isEmpty() const {
            return blocks.empty();
        }
    };

    class UapmdFunctionBlockManager {
        MidiIOManagerFeature* midi_io_manager{};
        std::vector<UapmdFunctionDevice> devices{};

    public:
        void setMidiIOManager(MidiIOManagerFeature* midiIOManager) { midi_io_manager = midiIOManager; }

        size_t count() const { return devices.size(); }

        size_t create() {
            // FIXME: this should not simply add a new device and return the simple size, because
            //  devices can be removed and then we will return the same index for different devices.
            devices.emplace_back(midi_io_manager);
            return devices.size() - 1;
        }

        UapmdFunctionDevice* getFunctionDeviceByIndex(const int32_t index) {
            return &devices[index];
        }

        // return the containing `UapmdFunctionDevice` of the `UapmdFunctionBlock` for the instance indicated by `instanceId`
        UapmdFunctionDevice* getFunctionDeviceForInstance(int32_t instanceId) {
            for (auto& block : devices) {
                for (auto& device : block.devices())
                    if (device->instanceId() == instanceId)
                        return &block;
            }
            return nullptr;
        }

        // FIXME: we should not really return shared_ptr here...
        std::shared_ptr<UapmdFunctionBlock> getFunctionDeviceByInstanceId(int32_t instanceId) {
            for (auto& block : devices) {
                if (auto ret = block.getDeviceByInstanceId(instanceId))
                    return ret;
            }
            return nullptr;
        }

        void deleteEmptyDevices() {
            devices.erase(
                std::remove_if(devices.begin(), devices.end(),
                    [](const UapmdFunctionDevice& block) { return block.isEmpty(); }),
                devices.end()
            );
        }
    };
}
