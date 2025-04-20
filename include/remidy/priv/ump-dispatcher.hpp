#pragma once

namespace remidy {
    class UmpInputDispatcher {
    public:
        virtual ~UmpInputDispatcher() = default;

        // generic processor
        virtual void process(uint64_t timestamp, AudioProcessContext& src) = 0;
    };

    typedef uint8_t uint4_t;
    typedef uint8_t uint7_t;

    class TypedUmpInputDispatcher : public UmpInputDispatcher {
        uint64_t _timestamp{0};
        TrackContext* track_context{}; // set every time process() is invoked

    protected:
        TrackContext* trackContext() { return track_context; }

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

        virtual void onProcessStart(AudioProcessContext& src) {}
        virtual void onProcessEnd(AudioProcessContext& src) {}
    public:
        void process(uint64_t timestamp, AudioProcessContext& src) override;

        uint64_t timestamp() { return _timestamp; }
    };
}
