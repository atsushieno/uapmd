#pragma once

#include <cstdint>
#include <cmath>

namespace uapmd_app {

    // Represents a position on the timeline
    struct TimelinePosition {
        int64_t samples{0};     // Primary representation (RT-safe)
        double beats{0.0};      // Secondary representation (for UI/musical time)

        TimelinePosition() = default;
        explicit TimelinePosition(int64_t s) : samples(s), beats(0.0) {}

        // Factory methods for creating positions
        static TimelinePosition fromSamples(int64_t samples, int32_t sampleRate, double tempo = 120.0) {
            TimelinePosition pos;
            pos.samples = samples;
            pos.beats = samplesToBeats(samples, tempo, sampleRate);
            return pos;
        }

        static TimelinePosition fromBeats(double beats, int32_t sampleRate, double tempo = 120.0) {
            TimelinePosition pos;
            pos.beats = beats;
            pos.samples = beatsToSamples(beats, tempo, sampleRate);
            return pos;
        }

        static TimelinePosition fromSeconds(double seconds, int32_t sampleRate, double tempo = 120.0) {
            TimelinePosition pos;
            pos.samples = static_cast<int64_t>(seconds * sampleRate);
            pos.beats = samplesToBeats(pos.samples, tempo, sampleRate);
            return pos;
        }

        // Conversion helpers
        static int64_t beatsToSamples(double beats, double tempo, int32_t sampleRate) {
            double secondsPerBeat = 60.0 / tempo;
            double seconds = beats * secondsPerBeat;
            return static_cast<int64_t>(seconds * sampleRate);
        }

        static double samplesToBeats(int64_t samples, double tempo, int32_t sampleRate) {
            double seconds = static_cast<double>(samples) / sampleRate;
            double beatsPerSecond = tempo / 60.0;
            return seconds * beatsPerSecond;
        }

        double toSeconds(int32_t sampleRate) const {
            return static_cast<double>(samples) / sampleRate;
        }

        // Comparison operators (based on samples)
        bool operator==(const TimelinePosition& other) const { return samples == other.samples; }
        bool operator!=(const TimelinePosition& other) const { return samples != other.samples; }
        bool operator<(const TimelinePosition& other) const { return samples < other.samples; }
        bool operator<=(const TimelinePosition& other) const { return samples <= other.samples; }
        bool operator>(const TimelinePosition& other) const { return samples > other.samples; }
        bool operator>=(const TimelinePosition& other) const { return samples >= other.samples; }
    };

    // Represents a single clip on a track
    struct ClipData {
        int32_t clipId{-1};
        TimelinePosition position;          // Position on timeline
        int64_t durationSamples{0};         // Duration of clip (full file length)
        int32_t sourceNodeInstanceId{-1};  // Which source node plays this clip

        // Playback properties
        double gain{1.0};
        bool muted{false};

        // Note: Clip regions, looping, automation, and time-stretch NOT included in this phase
        // Each clip plays the entire audio file from start to finish

        ClipData() = default;

        // Check if a timeline position falls within this clip's range
        bool contains(const TimelinePosition& pos) const {
            return pos.samples >= position.samples &&
                   pos.samples < position.samples + durationSamples;
        }

        // Get the position within the source file for a given timeline position
        int64_t getSourcePosition(const TimelinePosition& timelinePos) const {
            if (!contains(timelinePos))
                return -1;
            return timelinePos.samples - position.samples;
        }
    };

    // Global timeline state
    struct TimelineState {
        TimelinePosition playheadPosition;
        bool isPlaying{false};
        bool loopEnabled{false};
        TimelinePosition loopStart;
        TimelinePosition loopEnd;

        double tempo{120.0};                    // BPM
        int32_t timeSignatureNumerator{4};
        int32_t timeSignatureDenominator{4};

        TimelineState() = default;

        // Update playhead position based on frame count
        void advancePlayhead(int32_t frameCount, int32_t sampleRate) {
            if (!isPlaying)
                return;

            playheadPosition.samples += frameCount;

            // Handle loop region
            if (loopEnabled) {
                if (playheadPosition.samples >= loopEnd.samples) {
                    playheadPosition.samples = loopStart.samples;
                }
            }

            // Update beats representation
            playheadPosition.beats = TimelinePosition::samplesToBeats(
                playheadPosition.samples, tempo, sampleRate
            );
        }

        // Seek to a specific position
        void seekTo(const TimelinePosition& position, int32_t sampleRate) {
            playheadPosition = position;
            playheadPosition.beats = TimelinePosition::samplesToBeats(
                playheadPosition.samples, tempo, sampleRate
            );
        }
    };

} // namespace uapmd_app
