#pragma once
#include "uapmd/uapmd.hpp"

namespace uapmd {
    class UapmdFunctionBlock {
        MidiIOManagerFeature* midi_io_manager;
        std::map<int32_t,std::shared_ptr<UapmdMidiDevice>> devices_{};

    public:
        explicit UapmdFunctionBlock(MidiIOManagerFeature* midiIoManager) : midi_io_manager(midiIoManager) {}

        std::vector<UapmdMidiDevice*> devices() {
            std::vector<UapmdMidiDevice*> result{};
            for (auto &val: devices_ | std::views::values)
                if (val)  // Skip null shared_ptrs
                    result.push_back(val.get());
            return result;
        }

        bool createDevice(std::string& apiName,
                        SequencerFeature* sequencer,
                        int32_t instanceId,
                        int32_t trackIndex,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version) {
            std::shared_ptr<MidiIOFeature> midiDevice  = midi_io_manager->createMidiIOFeature(apiName, deviceName, manufacturer, version);
            if (!midiDevice) return false;

            devices_[instanceId] = std::make_shared<UapmdMidiDevice>(midiDevice, sequencer, instanceId, trackIndex, deviceName, manufacturer, version);
            return true;
        }

        // FIXME: we should not really return shared_ptr here...
        std::shared_ptr<UapmdMidiDevice> getDeviceByInstanceId(int32_t instanceId) {
            const auto it = devices_.find(instanceId);
            return it != devices_.end() ? it->second : nullptr;
        }

        void destroyDevice(const int32_t instanceId) {
            devices_.erase(instanceId);
        }

        bool isEmpty() const {
            return devices_.empty();
        }
    };

    class UapmdFunctionBlockManager {
        MidiIOManagerFeature* midi_io_manager{};
        std::vector<UapmdFunctionBlock> blocks{};

    public:
        void setMidiIOManager(MidiIOManagerFeature* midiIOManager) { midi_io_manager = midiIOManager; }

        size_t count() const { return blocks.size(); }

        size_t create() {
            blocks.emplace_back(midi_io_manager);
            return blocks.size() - 1;
        }

        UapmdFunctionBlock* getFunctionBlockByIndex(const int32_t index) {
            return &blocks[index];
        }

        UapmdFunctionBlock* getFunctionBlockForInstance(int32_t instanceId) {
            for (auto& block : blocks) {
                for (auto& device : block.devices())
                    if (device->instanceId() == instanceId)
                        return &block;
            }
            return nullptr;
        }

        // FIXME: we should not really return shared_ptr here...
        std::shared_ptr<UapmdMidiDevice> getDeviceByInstanceId(int32_t instanceId) {
            for (auto& block : blocks) {
                if (auto ret = block.getDeviceByInstanceId(instanceId))
                    return ret;
            }
            return nullptr;
        }

        void deleteEmptyBlocks() {
            blocks.erase(
                std::remove_if(blocks.begin(), blocks.end(),
                    [](const UapmdFunctionBlock& block) { return block.isEmpty(); }),
                blocks.end()
            );
        }
    };
}
