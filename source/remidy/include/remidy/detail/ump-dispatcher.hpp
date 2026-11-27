#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace remidy {
    // Reassembles SysEx7 UMP packets per group into F0..F7 framed MIDI 1.0 SysEx bytes.
    class Sysex7Assembler {
    public:
        static constexpr size_t MAX_MESSAGE_SIZE = 1024;

        // Returns true with the complete message, which stays valid until the next start packet of the same group.
        bool feed(uint32_t word0, uint32_t word1, const uint8_t*& data, size_t& size);

    private:
        struct GroupState {
            std::array<uint8_t, MAX_MESSAGE_SIZE> buffer{};
            size_t size{0};
            bool overflow{false};
        };
        std::array<GroupState, 16> groups{};
    };

    class UmpInputDispatcher {
    public:
        virtual ~UmpInputDispatcher() = default;

        // generic processor
        virtual void process(AudioProcessContext& src) = 0;
    };

    typedef uint8_t uint4_t;
    typedef uint8_t uint7_t;

    class TypedUmpInputDispatcher : public UmpInputDispatcher {
        uint64_t _jr_timestamp_ticks{0};
        MasterContext* master_context;
        Sysex7Assembler sysex7_assembler{};

    protected:
        // 80h
        virtual void onNoteOff(uint4_t group, uint4_t channel, uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {}
        // 90h
        virtual void onNoteOn(uint4_t group, uint4_t channel, uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {}
        // A0h (Poly), D0 (Channel)
        virtual void onPressure(uint4_t group, uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {}
        // B0h
        virtual void onCC(uint4_t group, uint4_t channel, uint7_t index, uint32_t data) {}
        // C0h
        virtual void onProgramChange(uint4_t group, uint4_t channel, uint7_t flags, uint7_t program, uint7_t bankMSB, uint7_t bankLSB) {}
        // E0h, 60h (PN)
        virtual void onPitchBend(uint4_t group, uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {}
        // F0
        virtual void onPerNoteManagement(uint4_t group, uint4_t channel, uint7_t note, uint8_t flags) {}
        // 00h
        virtual void onPNRC(uint4_t group, uint4_t channel, uint7_t note, uint8_t index, uint32_t data) {}
        // 10h
        virtual void onPNAC(uint4_t group, uint4_t channel, uint7_t note, uint8_t index, uint32_t data) {}
        // 20h, 40h (rel)
        virtual void onRC(uint4_t group, uint4_t channel, uint7_t bank, uint7_t index, uint32_t data, bool relative) {}
        // 30h, 50h (rel)
        virtual void onAC(uint4_t group, uint4_t channel, uint7_t bank, uint7_t index, uint32_t data, bool relative) {}
        // SysEx7 (message type 3), reassembled into F0..F7 framed bytes
        virtual void onSysEx7(uint4_t group, const uint8_t* data, size_t size) {}

        virtual void onProcessStart(AudioProcessContext& src) {}
        virtual void onProcessEnd(AudioProcessContext& src) {}
    public:
        void process(AudioProcessContext& src) override;

        // The position of the current event within the block, in samples.
        uint64_t timestamp() {
            return master_context ? _jr_timestamp_ticks * static_cast<uint64_t>(master_context->sampleRate()) / 31250 : 0;
        }
        MasterContext* masterContext() { return master_context; }
    };
}
