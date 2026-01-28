#pragma once

#include <remidy/remidy.hpp>

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

        // UAPMD maps a parameter ID to a per-note assignable controller index, which is only 7 bytes.
        // (There is no relative PN-AC, so no getter here.)
        virtual void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) = 0;

        // Unlike Assignable Controllers, We use bank MSB, LSB and program index, which totals to 24-bits.
        virtual void loadPreset(uint32_t index) = 0;
    };

    class UapmdUmpOutputMapper {
    public:
        virtual ~UapmdUmpOutputMapper() = default;

        virtual void sendParameterValue(uint16_t index, double value) = 0;
        virtual void sendPerNoteControllerValue(uint8_t note, uint8_t index, double value) = 0;
        virtual void sendPresetIndexChange(uint32_t index) = 0;
    };
}