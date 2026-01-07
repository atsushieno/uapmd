
#include <algorithm>
#include <limits>
#include "cmidi2.h"
#include "uapmd/priv/devices/MidiIODevice.hpp"
#include "UapmdNodeUmpMapper.hpp"

namespace uapmd {
    UapmdNodeUmpInputMapper::UapmdNodeUmpInputMapper(AudioPluginNodeAPI* plugin)
      : UapmdUmpInputMapper(),
        plugin(plugin) {
    }

    void UapmdNodeUmpInputMapper::process(uint64_t timestamp, remidy::AudioProcessContext& src) {
        UapmdUmpInputMapper::process(timestamp, src);
    }

    // UAPMD maps a parameter ID to an assignable controller bank and index, which totals only up to 14 bytes.
    // We use uint16_t to match that.
    void UapmdNodeUmpInputMapper::setParameterValue(uint16_t index, double value) {
        plugin->setParameterValue(index, value);
    }

    // Same for uint16_t index as `setParameter()`.
    // We use this function to calculate "relative assignable controllers"
    double UapmdNodeUmpInputMapper::getParameterValue(uint16_t index) {
        return plugin->getParameterValue(index);
    }

    void UapmdNodeUmpInputMapper::setPerNoteControllerValue(uint8_t note, uint8_t index, double value) {
        plugin->setPerNoteControllerValue(note, index, value);
    }

    // Unlike Assignable Controllers, We use bank MSB, LSB and program index, which totals to 24-bits.
    void UapmdNodeUmpInputMapper::loadPreset(uint32_t index) {
        plugin->loadPreset(index);
    }

    UapmdNodeUmpOutputMapper::UapmdNodeUmpOutputMapper(std::shared_ptr<MidiIODevice> device, AudioPluginNodeAPI* plugin)
      : UapmdUmpOutputMapper(),
        device(std::move(device)),
        plugin(plugin) {
        param_change_listener_id = plugin->addParameterChangeListener([this](uint32_t index, double value) {
            onParameterValueUpdated(index, value);
        });
    }

    UapmdNodeUmpOutputMapper::~UapmdNodeUmpOutputMapper() {
        if (plugin && param_change_listener_id != 0)
            plugin->removeParameterChangeListener(param_change_listener_id);
    }

    void UapmdNodeUmpOutputMapper::onParameterValueUpdated(uint16_t index, double value) {
        if (!device)
            return;
        constexpr uint8_t group = 0;
        constexpr uint8_t channel = 0;
        const uint8_t bank = static_cast<uint8_t>((index >> 7) & 0x7F);
        const uint8_t controllerIndex = static_cast<uint8_t>(index & 0x7F);
        const double clamped = std::clamp(value, 0.0, 1.0);
        const auto data = static_cast<uint32_t>(clamped * static_cast<double>(std::numeric_limits<uint32_t>::max()));
        auto ump = cmidi2_ump_midi2_nrpn(group, channel, bank, controllerIndex, data);
        uapmd_ump_t words[2]{
            static_cast<uapmd_ump_t>(ump >> 32),
            static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu)
        };
        device->send(words, sizeof(words), 0);
    }

    void UapmdNodeUmpOutputMapper::onPresetLoaded(uint32_t index) {
        if (!device)
            return;
        constexpr uint8_t group = 0;
        constexpr uint8_t channel = 0;
        const uint8_t program = static_cast<uint8_t>(index & 0x7F);
        const uint16_t bankIndex = static_cast<uint16_t>(index >> 7);
        const uint8_t bankMsb = static_cast<uint8_t>((bankIndex >> 7) & 0x7F);
        const uint8_t bankLsb = static_cast<uint8_t>(bankIndex & 0x7F);
        auto ump = cmidi2_ump_midi2_program(group, channel, 0, program, bankMsb, bankLsb);
        uapmd_ump_t words[2]{
            static_cast<uapmd_ump_t>(ump >> 32),
            static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu)
        };
        device->send(words, sizeof(words), 0);
    }
}
