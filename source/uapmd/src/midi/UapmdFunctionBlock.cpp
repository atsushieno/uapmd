
#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>

#include <midicci/midicci.hpp>
#include "uapmd/uapmd.hpp"
#include "UapmdMidiCISession.hpp"
#include "UapmdNodeUmpMapper.hpp"

using namespace midicci::commonproperties;

namespace uapmd {

    std::vector<UapmdFunctionBlock*> UapmdFunctionDevice::devices() {
        std::vector<UapmdFunctionBlock*> result{};
        for (auto &val: blocks | std::views::values)
            if (val)  // Skip null shared_ptrs
                result.push_back(val.get());
        return result;
    }

    bool UapmdFunctionDevice::createFunctionBlock(const std::string& apiName,
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

    UapmdFunctionBlock::UapmdFunctionBlock(std::shared_ptr<MidiIOFeature> midiDevice,
                                     AudioPluginNode* pluginNode,
                                     std::string deviceName,
                                     std::string manufacturerName,
                                     std::string versionString)
      : plugin_node(pluginNode),
        midi_device(std::move(midiDevice)) {
        uapmd_sessions = UapmdMidiCISession::create(this, pluginNode->instance(), deviceName, manufacturerName, versionString);
        if (midi_device && plugin_node && plugin_node->instance())
            ump_output_mapper_ = std::make_unique<UapmdNodeUmpOutputMapper>(midi_device.get(), plugin_node->instance());
        if (midi_device)
            midi_device->addInputHandler(umpReceived, this);
    }

    UapmdFunctionBlock::~UapmdFunctionBlock() {
        if (midi_device)
            midi_device->removeInputHandler(umpReceived);
    }

    void UapmdFunctionBlock::detachOutputMapper() {
        if (ump_output_mapper_)
            ump_output_mapper_->detach();
        ump_output_mapper_.reset();
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
