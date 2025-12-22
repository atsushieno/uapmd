#pragma once
#include <remidy/remidy.hpp>
#include "../../../include/uapmd/priv/midi/UapmdUmpMapper.hpp"

namespace uapmd {
    class UapmdNodeUmpInputMapper :
        public UapmdUmpInputMapper,
        public remidy::UmpInputDispatcher {
        AudioPluginHostPAL::AudioPluginNodePAL* plugin;

    public:
        explicit UapmdNodeUmpInputMapper(AudioPluginHostPAL::AudioPluginNodePAL* plugin);

        void process(uint64_t timestamp, remidy::AudioProcessContext& src) override;

        // UAPMD maps a parameter ID to an assignable controller bank and index, which totals only up to 14 bytes.
        // We use uint16_t to match that.
        void setParameterValue(uint16_t index, double value) override;

        // Same for uint16_t index as `setParameter()`.
        // We use this function to calculate "relative assignable controllers"
        double getParameterValue(uint16_t index) override;

        // Unlike Assignable Controllers, We use bank MSB, LSB and program index, which totals to 24-bits.
        void loadPreset(uint32_t index) override;
    };
}
