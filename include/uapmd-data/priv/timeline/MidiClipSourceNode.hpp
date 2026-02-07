#pragma once

#include "MidiSourceNode.hpp"
#include <uapmd/uapmd.hpp>
#include <vector>
#include <atomic>
#include <cstdint>

namespace uapmd {

    // Concrete implementation of MIDI clip playback
    // Pre-loads UMP events and generates them sample-accurately during playback
    class MidiClipSourceNode : public MidiSourceNode {
    public:
        // Constructor
        // umpEvents: Pre-converted UMP messages from SMF
        // umpTickTimestamps: Cumulative ticks for each UMP word
        // tickResolution: Ticks per quarter note from SMF header
        // clipTempo: Original tempo from SMF (BPM)
        // targetSampleRate: Project sample rate
        MidiClipSourceNode(
            int32_t instanceId,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            double targetSampleRate
        );

        ~MidiClipSourceNode() override = default;

        // SourceNode interface
        int32_t instanceId() const override { return instance_id_; }
        SourceNodeType nodeType() const override { return SourceNodeType::MidiClipSource; }
        bool bypassed() const override { return bypassed_; }
        void bypassed(bool value) override { bypassed_ = value; }
        std::vector<uint8_t> saveState() override;
        void loadState(const std::vector<uint8_t>& state) override;

        // MidiSourceNode interface
        void seek(int64_t samplePosition) override;
        int64_t currentPosition() const override;
        int64_t totalLength() const override;
        bool isPlaying() const override;
        void setPlaying(bool playing) override;
        void processEvents(
            remidy::EventSequence& eventOut,
            int32_t frameCount,
            int32_t sampleRate,
            double tempo
        ) override;

    private:
        int32_t instance_id_;
        bool bypassed_{false};
        std::atomic<bool> is_playing_{false};
        std::atomic<int64_t> playback_position_{0};  // In samples

        // MIDI clip data
        std::vector<uapmd_ump_t> ump_events_;           // Pre-loaded UMP messages
        std::vector<uint64_t> event_timestamps_samples_; // Converted to samples
        uint32_t tick_resolution_;
        double clip_tempo_;
        double target_sample_rate_;
        int64_t total_length_samples_{0};

        // Playback state
        std::atomic<size_t> next_event_index_{0};  // Next event to emit

        // Helper: append UMP to EventSequence
        void appendUmpToEventSequence(
            remidy::EventSequence& seq,
            uapmd_ump_t ump,
            uint32_t frameOffset
        );

        // Helper: get UMP message size in bytes
        static size_t getUmpMessageSizeInBytes(uapmd_ump_t ump);
    };

} // namespace uapmd
