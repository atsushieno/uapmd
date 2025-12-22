#pragma once

#include <remidy/remidy.hpp>
#include <uapmd/priv/plugingraph/AudioPluginHostPAL.hpp>

namespace uapmd {
    // Process UAPMD-intrinsic UMP mappings, in particular:
    // - assignable controllers (NRPNs) to plugin parameters
    // - program/bank change to plugin presets (by index)
    //
    // It makes use of UmpInputDispatcher in uapmd implementation.
    //
    // <del>Any unmapped UMPs are copied into output events.</del>
    // We so far skip and pass unmodified sources to the plugin later.
    class UapmdUmpInputMapper {

    public:
        virtual ~UapmdUmpInputMapper() = default;

        virtual void process(uint64_t timestamp, remidy::AudioProcessContext& src) = 0;

        // UAPMD maps a parameter ID to an assignable controller bank and index, which totals only up to 14 bytes.
        // We use uint16_t to match that.
        virtual void setParameterValue(uint16_t index, double value) = 0;

        // Same for uint16_t index as `setParameter()`.
        // We use this function to calculate "relative assignable controllers"
        virtual double getParameterValue(uint16_t index) = 0;

        // Unlike Assignable Controllers, We use bank MSB, LSB and program index, which totals to 24-bits.
        virtual void loadPreset(uint32_t index) = 0;

        // FIXME: add support for per-note controllers
    };

}