#include <algorithm>
#include <cstring>
#include <umppi/umppi.hpp>
#include <remidy/remidy.hpp>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

    MidiClipSourceNode::MidiClipSourceNode(
        int32_t instanceId,
        std::vector<uapmd_ump_t> umpEvents,
        std::vector<uint64_t> umpTickTimestamps,
        uint32_t tickResolution,
        double clipTempo,
        double targetSampleRate,
        std::vector<MidiTempoChange> tempoChanges,
        std::vector<MidiTimeSignatureChange> timeSignatureChanges
    ) : instance_id_(instanceId),
        ump_events_(std::move(umpEvents)),
        event_timestamps_ticks_(std::move(umpTickTimestamps)),
        tempo_changes_(std::move(tempoChanges)),
        time_signature_changes_(std::move(timeSignatureChanges)),
        tick_resolution_(tickResolution == 0 ? 480 : tickResolution),
        clip_tempo_(clipTempo <= 0.0 ? 120.0 : clipTempo),
        target_sample_rate_(targetSampleRate)
    {
        // Ensure tempo changes start at tick 0 and are ordered
        auto tempoComparator = [](const MidiTempoChange& a, const MidiTempoChange& b) {
            return a.tickPosition < b.tickPosition;
        };
        if (tempo_changes_.empty()) {
            tempo_changes_.push_back(MidiTempoChange{0, clip_tempo_});
        } else {
            std::sort(tempo_changes_.begin(), tempo_changes_.end(), tempoComparator);
            if (tempo_changes_.front().tickPosition > 0) {
                tempo_changes_.insert(
                    tempo_changes_.begin(),
                    MidiTempoChange{0, tempo_changes_.front().bpm > 0.0 ? tempo_changes_.front().bpm : clip_tempo_}
                );
            }
            for (auto& change : tempo_changes_) {
                if (change.bpm <= 0.0)
                    change.bpm = clip_tempo_;
            }
        }

        auto timeSigComparator = [](const MidiTimeSignatureChange& a, const MidiTimeSignatureChange& b) {
            return a.tickPosition < b.tickPosition;
        };
        if (time_signature_changes_.empty()) {
            time_signature_changes_.push_back(MidiTimeSignatureChange{0, 4, 4});
        } else {
            std::sort(time_signature_changes_.begin(), time_signature_changes_.end(), timeSigComparator);
            if (time_signature_changes_.front().tickPosition > 0) {
                time_signature_changes_.insert(
                    time_signature_changes_.begin(),
                    MidiTimeSignatureChange{
                        0,
                        time_signature_changes_.front().numerator,
                        time_signature_changes_.front().denominator
                    }
                );
            }
        }

        rebuildSampleTimelines();
    }

    std::vector<uint8_t> MidiClipSourceNode::saveState() {
        // Not implemented yet - MIDI clips don't have runtime state to save
        return {};
    }

    void MidiClipSourceNode::loadState(const std::vector<uint8_t>& state) {
        // Not implemented yet
    }

    void MidiClipSourceNode::seek(int64_t samplePosition) {
        playback_position_.store(samplePosition, std::memory_order_release);

        // Binary search for first event at or after samplePosition
        auto it = std::lower_bound(
            event_timestamps_samples_.begin(),
            event_timestamps_samples_.end(),
            static_cast<uint64_t>(samplePosition)
        );

        size_t index = std::distance(event_timestamps_samples_.begin(), it);
        next_event_index_.store(index, std::memory_order_release);
    }

    int64_t MidiClipSourceNode::currentPosition() const {
        return playback_position_.load(std::memory_order_acquire);
    }

    int64_t MidiClipSourceNode::totalLength() const {
        return total_length_samples_;
    }

    bool MidiClipSourceNode::isPlaying() const {
        return is_playing_.load(std::memory_order_acquire);
    }

    void MidiClipSourceNode::setPlaying(bool playing) {
        is_playing_.store(playing, std::memory_order_release);
    }

    void MidiClipSourceNode::processEvents(
        remidy::EventSequence& eventOut,
        int32_t frameCount,
        int32_t sampleRate,
        double tempo
    ) {
        if (bypassed_ || !is_playing_.load(std::memory_order_acquire))
            return;

        int64_t currentPos = playback_position_.load(std::memory_order_acquire);
        int64_t windowEnd = currentPos + frameCount;
        size_t eventIdx = next_event_index_.load(std::memory_order_acquire);

        // Emit all events within [currentPos, windowEnd)
        while (eventIdx < ump_events_.size()) {
            uint64_t eventSamples = event_timestamps_samples_[eventIdx];

            // Beyond current window
            if (eventSamples >= static_cast<uint64_t>(windowEnd))
                break;

            // Within window - emit event
            if (eventSamples >= static_cast<uint64_t>(currentPos)) {
                uint32_t frameOffset = static_cast<uint32_t>(eventSamples - currentPos);
                appendUmpToEventSequence(eventOut, ump_events_[eventIdx], frameOffset);
            }

            eventIdx++;
        }

        next_event_index_.store(eventIdx, std::memory_order_release);
    }


    void MidiClipSourceNode::appendUmpToEventSequence(
        remidy::EventSequence& seq,
        uapmd_ump_t ump,
        uint32_t frameOffset
    ) {
        size_t pos = seq.position();
        auto* buffer = static_cast<uint32_t*>(seq.getMessages());
        size_t maxSize = seq.maxMessagesInBytes();
        size_t umpPosition = pos / sizeof(uint32_t);
        size_t umpCapacity = maxSize / sizeof(uint32_t);

        // Write JR_TIMESTAMP message for sample-accurate timing
        // JR_TIMESTAMP uses 31250 ticks per second, so convert frame offset to JR ticks
        // For now, use a simple approach: write the frame offset directly as ticks
        // (This assumes the receiver will convert appropriately)
        if (frameOffset > 0 && umpPosition < umpCapacity) {
            // Create JR_TIMESTAMP utility message (0x00 20 xxxx)
            // MessageType=0 (utility), Status=0x20 (JR_TIMESTAMP), Data=frameOffset (16-bit)
            uint32_t jrTimestamp = (0x0 << 28) | (0x20 << 16) | (frameOffset & 0xFFFF);
            buffer[umpPosition++] = jrTimestamp;
        }

        // Get UMP message size in words
        size_t umpSizeInWords = getUmpMessageSizeInBytes(ump) / sizeof(uint32_t);

        // Check buffer space
        if (umpPosition + umpSizeInWords > umpCapacity)
            return; // Buffer full - drop event

        // Write UMP data as uint32_t words
        // Convert uapmd_ump_t (128-bit) to individual uint32_t words
        const auto* umpWords = reinterpret_cast<const uint32_t*>(&ump);
        for (size_t i = 0; i < umpSizeInWords; i++) {
            buffer[umpPosition++] = umpWords[i];
        }

        seq.position(umpPosition * sizeof(uint32_t));
    }

    size_t MidiClipSourceNode::getUmpMessageSizeInBytes(uapmd_ump_t ump) {
        // Determine UMP message size from message type (high nibble)
        uint8_t messageType = (ump >> 28) & 0xF;

        switch (messageType) {
            case 0: // Utility messages (32-bit)
            case 1: // System Real Time messages (32-bit)
            case 2: // MIDI 1.0 Channel Voice messages (32-bit)
                return 4;

            case 3: // Data messages (64-bit)
            case 4: // MIDI 2.0 Channel Voice messages (64-bit)
                return 8;

            case 5: // Data messages (128-bit)
            case 0xD: // Flex Data messages (128-bit)
            case 0xF: // Stream messages (128-bit)
                return 16;

            default:
                return 4; // Default to 32-bit
        }
    }

    void MidiClipSourceNode::rebuildSampleTimelines() {
        event_timestamps_samples_ = computeSampleTimeline(event_timestamps_ticks_);

        std::vector<uint64_t> tempoTicks;
        tempoTicks.reserve(tempo_changes_.size());
        for (const auto& change : tempo_changes_) {
            tempoTicks.push_back(change.tickPosition);
        }
        tempo_change_samples_ = computeSampleTimeline(tempoTicks);

        std::vector<uint64_t> timeSigTicks;
        timeSigTicks.reserve(time_signature_changes_.size());
        for (const auto& change : time_signature_changes_) {
            timeSigTicks.push_back(change.tickPosition);
        }
        time_signature_change_samples_ = computeSampleTimeline(timeSigTicks);

        total_length_samples_ = 0;
        for (uint64_t samples : event_timestamps_samples_) {
            if (static_cast<int64_t>(samples) > total_length_samples_) {
                total_length_samples_ = static_cast<int64_t>(samples);
            }
        }
    }

    std::vector<uint64_t> MidiClipSourceNode::computeSampleTimeline(const std::vector<uint64_t>& ticks) const {
        std::vector<uint64_t> samples;
        if (ticks.empty() || tempo_changes_.empty())
            return samples;

        samples.reserve(ticks.size());

        size_t tempoIndex = 0;
        uint64_t processedTick = tempo_changes_[tempoIndex].tickPosition;
        if (processedTick > 0 && processedTick > ticks.front())
            processedTick = ticks.front();

        double bpm = tempo_changes_[tempoIndex].bpm > 0.0 ? tempo_changes_[tempoIndex].bpm : clip_tempo_;
        double secondsPerTick = (60.0 / bpm) / static_cast<double>(tick_resolution_);
        double secondsAccum = 0.0;

        for (uint64_t tick : ticks) {
            while (tempoIndex + 1 < tempo_changes_.size() &&
                   tempo_changes_[tempoIndex + 1].tickPosition <= tick) {
                uint64_t changeTick = tempo_changes_[tempoIndex + 1].tickPosition;
                if (changeTick > processedTick) {
                    secondsAccum += static_cast<double>(changeTick - processedTick) * secondsPerTick;
                    processedTick = changeTick;
                }
                ++tempoIndex;
                double nextBpm = tempo_changes_[tempoIndex].bpm;
                if (nextBpm <= 0.0)
                    nextBpm = clip_tempo_;
                secondsPerTick = (60.0 / nextBpm) / static_cast<double>(tick_resolution_);
            }

            if (tick > processedTick) {
                secondsAccum += static_cast<double>(tick - processedTick) * secondsPerTick;
                processedTick = tick;
            }

            samples.push_back(static_cast<uint64_t>(secondsAccum * target_sample_rate_));
        }

        return samples;
    }

} // namespace uapmd
