#pragma once
#include "UapmdMidiDevice.hpp"

namespace uapmd {
    class UapmdFunctionBlock {
        MidiIOManagerFeature* midi_io_manager;
        std::map<int32_t,std::shared_ptr<UapmdMidiDevice>> devices_{};

    public:
        explicit UapmdFunctionBlock(MidiIOManagerFeature* midiIoManager) : midi_io_manager(midiIoManager) {}

        std::vector<UapmdMidiDevice*> devices() {
            std::vector<UapmdMidiDevice*> result{};
            for (auto &val: devices_ | std::views::values)
                result.push_back(val.get());
            return result;
        }

        int32_t createDevice(std::shared_ptr<MidiIOFeature> midiDevice,
                        SequencerFeature* sequencer,
                        int32_t instanceId,
                        int32_t trackIndex,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version) {
            devices_[instanceId] = std::make_shared<UapmdMidiDevice>(midiDevice, sequencer, instanceId, trackIndex, deviceName, manufacturer, version);
            return instanceId;
        }

        // FIXME: we should not really return shared_ptr here...
        std::shared_ptr<UapmdMidiDevice> getDeviceById(int32_t instanceId) {
            return devices_[instanceId];
        }

        void destroyDevice(const int32_t instanceId) {
            devices_[instanceId].reset();
        }
    };

    class UapmdFunctionBlockManager {
        MidiIOManagerFeature* midi_io_manager;
        std::vector<UapmdFunctionBlock> blocks{};

    public:
        explicit UapmdFunctionBlockManager(MidiIOManagerFeature* midiIOManager) : midi_io_manager(midiIOManager) {}

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
        std::shared_ptr<UapmdMidiDevice> getDeviceById(int32_t instanceId) {
            for (auto& block : blocks) {
                if (auto ret = block.getDeviceById(instanceId))
                    return ret;
            }
            return nullptr;
        }
    };
}
