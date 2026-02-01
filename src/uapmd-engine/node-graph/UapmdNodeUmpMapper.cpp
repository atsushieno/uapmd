
#include <algorithm>
#include <cmath>
#include <limits>
#include <umppi/umppi.hpp>
#include "uapmd/uapmd.hpp"
#include "UapmdNodeUmpMapper.hpp"

namespace uapmd {
    UapmdNodeUmpInputMapper::UapmdNodeUmpInputMapper(AudioPluginInstanceAPI* plugin)
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

    UapmdNodeUmpOutputMapper::UapmdNodeUmpOutputMapper(MidiIOFeature* device, AudioPluginInstanceAPI* plugin)
      : UapmdUmpOutputMapper(),
        device(device),
        plugin(plugin),
        parameter_support(plugin ? plugin->parameterSupport() : nullptr),
        param_change_listener_id(-1),
        per_note_change_listener_id(-1) {
        if (!parameter_support)
            return;
        param_change_listener_id = parameter_support->parameterChangeEvent().addListener([this](uint32_t index, double value) {
            sendParameterValue(index, value);
        });
        per_note_change_listener_id = parameter_support->perNoteControllerChangeEvent().addListener(
            [this](remidy::PerNoteControllerContextTypes types, uint32_t context, uint32_t parameterIndex, double value) {
                if ((types & remidy::PER_NOTE_CONTROLLER_PER_NOTE) == 0)
                    return;
                const double normalized = normalizePerNoteControllerValue(types, context, parameterIndex, value);
                sendPerNoteControllerValue(
                    static_cast<uint8_t>(context),
                    static_cast<uint8_t>(parameterIndex),
                    normalized);
            });
    }

    UapmdNodeUmpOutputMapper::~UapmdNodeUmpOutputMapper() {
        if (plugin && parameter_support) {
            if (param_change_listener_id >= 0)
                parameter_support->parameterChangeEvent().removeListener(param_change_listener_id);
            if (per_note_change_listener_id >= 0)
                parameter_support->perNoteControllerChangeEvent().removeListener(per_note_change_listener_id);
        }
    }

    void UapmdNodeUmpOutputMapper::sendParameterValue(uint16_t index, double value) {
        if (!device)
            return;
        if (index >= 1 << 14)
            return;
        double normalized = normalizeParameterValue(index, value);
        if (!std::isfinite(normalized))
            normalized = 0.0;
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        constexpr uint8_t group = 0;
        constexpr uint8_t channel = 0;
        const uint8_t bank = static_cast<uint8_t>((index >> 7) & 0x7F);
        const uint8_t controllerIndex = static_cast<uint8_t>(index & 0x7F);
        const auto data = static_cast<uint32_t>(clamped * static_cast<double>(std::numeric_limits<uint32_t>::max()));
        auto ump = umppi::UmpFactory::midi2NRPN(group, channel, bank, controllerIndex, data);
        uapmd_ump_t words[2]{
            static_cast<uapmd_ump_t>(ump >> 32),
            static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu)
        };
        device->send(words, sizeof(words), 0);
    }

    void UapmdNodeUmpOutputMapper::sendPerNoteControllerValue(uint8_t note, uint8_t index, double value) {
        if (!device)
            return;
        if (index >= 1 << 7)
            return;
        double normalized = value;
        if (!std::isfinite(normalized))
            normalized = 0.0;
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        constexpr uint8_t group = 0;
        constexpr uint8_t channel = 0;
        const auto data = static_cast<uint32_t>(clamped * static_cast<double>(std::numeric_limits<uint32_t>::max()));
        auto ump = umppi::UmpFactory::midi2PerNoteACC(group, channel, note, index, data);
        uapmd_ump_t words[2]{
            static_cast<uapmd_ump_t>(ump >> 32),
            static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu)
        };
        device->send(words, sizeof(words), 0);
    }

    void UapmdNodeUmpOutputMapper::sendPresetIndexChange(uint32_t index) {
        if (!device)
            return;
        constexpr uint8_t group = 0;
        constexpr uint8_t channel = 0;
        const uint8_t program = static_cast<uint8_t>(index & 0x7F);
        const uint16_t bankIndex = static_cast<uint16_t>(index >> 7);
        const uint8_t bankMsb = static_cast<uint8_t>((bankIndex >> 7) & 0x7F);
        const uint8_t bankLsb = static_cast<uint8_t>(bankIndex & 0x7F);
        auto ump = umppi::UmpFactory::midi2Program(group, channel, 0, program, bankMsb, bankLsb);
        uapmd_ump_t words[2]{
            static_cast<uapmd_ump_t>(ump >> 32),
            static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu)
        };
        device->send(words, sizeof(words), 0);
    }

    double UapmdNodeUmpOutputMapper::normalizeParameterValue(uint16_t index, double plainValue) const {
        if (!parameter_support)
            return plainValue;
        auto& params = parameter_support->parameters();
        if (index >= params.size())
            return plainValue;
        auto* param = params[index];
        if (!param)
            return plainValue;
        double normalized = param->normalizedValue(plainValue);
        if (!std::isfinite(normalized))
            return 0.0;
        return normalized;
    }

    double UapmdNodeUmpOutputMapper::normalizePerNoteControllerValue(remidy::PerNoteControllerContextTypes types, uint32_t context, uint32_t parameterIndex, double plainValue) const {
        if (!parameter_support)
            return plainValue;
        remidy::PerNoteControllerContext ctx{};
        ctx.note = context;
        auto& controllers = parameter_support->perNoteControllers(types, ctx);
        if (parameterIndex >= controllers.size())
            return plainValue;
        auto* param = controllers[parameterIndex];
        if (!param)
            return plainValue;
        double normalized = param->normalizedValue(plainValue);
        if (!std::isfinite(normalized))
            return 0.0;
        return normalized;
    }
}
