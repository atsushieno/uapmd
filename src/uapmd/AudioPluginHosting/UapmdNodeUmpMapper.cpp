#include "UapmdNodeUmpMapper.hpp"

#include "uapmd/priv/devices/MidiIODevice.hpp"
#include "uapmd/priv/midi/PlatformVirtualMidiDevice.hpp"

namespace uapmd {
    UapmdNodeUmpInputMapper::UapmdNodeUmpInputMapper(AudioPluginHostPAL::AudioPluginNodePAL* plugin)
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

    // Unlike Assignable Controllers, We use bank MSB, LSB and program index, which totals to 24-bits.
    void UapmdNodeUmpInputMapper::loadPreset(uint32_t index) {
        plugin->loadPreset(index);
    }

    UapmdNodeUmpOutputMapper::UapmdNodeUmpOutputMapper(MidiIODevice* device, AudioPluginHostPAL::AudioPluginNodePAL* plugin)
      : UapmdUmpOutputMapper(),
        device(device),
        plugin(plugin) {
        param_change_listener_id = plugin->addParameterChangeListener([&](uint32_t index, double value) {
            onParameterValueUpdated(index, value);
        });
    }

    void UapmdNodeUmpOutputMapper::onParameterValueUpdated(uint16_t index, double value) {

        // FIXME: send AC
    }

    void UapmdNodeUmpOutputMapper::onPresetLoaded(uint32_t index) {
        // FIXME: send program change
    }
}
