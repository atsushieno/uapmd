#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>

namespace uapmd_app {

    // Anchor origin - whether the anchor point is at the start or end of a clip/track
    enum class AnchorOrigin {
        Start,  // Anchor at the beginning of the clip/track
        End     // Anchor at the end of the clip/track
    };

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

        // Arithmetic operators (for anchor offset calculations)
        TimelinePosition operator+(const TimelinePosition& other) const {
            TimelinePosition result;
            result.samples = samples + other.samples;
            result.beats = beats + other.beats;
            return result;
        }

        TimelinePosition operator-(const TimelinePosition& other) const {
            TimelinePosition result;
            result.samples = samples - other.samples;
            result.beats = beats - other.beats;
            return result;
        }
    };

    // Represents a single clip on a track
    struct ClipData {
        int32_t clipId{-1};
        TimelinePosition position;          // Absolute position on timeline (calculated from anchor)
        int64_t durationSamples{0};         // Duration of clip (full file length)
        int32_t sourceNodeInstanceId{-1};  // Which source node plays this clip

        // Playback properties
        double gain{1.0};
        bool muted{false};
        std::string name;                   // User-editable clip name
        std::string filepath;               // Source audio file path

        // Anchor support (NEW)
        int32_t anchorClipId{-1};          // -1 = track anchor (absolute position), >= 0 = clip ID
        AnchorOrigin anchorOrigin{AnchorOrigin::Start};  // Whether anchor is at start or end of reference
        TimelinePosition anchorOffset;      // Offset from anchor position

        // Note: Clip regions, looping, automation, and time-stretch NOT included in this phase
        // Each clip plays the entire audio file from start to finish

        ClipData() = default;

        // Check if a timeline position falls within this clip's range
        bool contains(const TimelinePosition& pos) const {
            return pos.samples >= position.samples &&
                   pos.samples < position.samples + durationSamples;
        }

        // Get the position within the source file for a given timeline position
        // This version uses the stored position field (only valid if position is up-to-date)
        int64_t getSourcePosition(const TimelinePosition& timelinePos) const {
            if (!contains(timelinePos))
                return -1;
            return timelinePos.samples - position.samples;
        }

        // Get the position within the source file, calculating absolute position from anchors
        int64_t getSourcePosition(const TimelinePosition& timelinePos, const std::unordered_map<int32_t, const ClipData*>& clips) const {
            TimelinePosition absPos = getAbsolutePosition(clips);
            if (timelinePos.samples < absPos.samples ||
                timelinePos.samples >= absPos.samples + durationSamples)
                return -1;
            return timelinePos.samples - absPos.samples;
        }

        // Helper: Calculate absolute position from anchor
        // For RT-safe queries, use TrackClipManager's cached resolution
        TimelinePosition getAbsolutePosition(const std::unordered_map<int32_t, const ClipData*>& clips) const {
            if (anchorClipId == -1) {
                // Track anchor - anchorOffset is absolute position
                // anchorOrigin is meaningless for track anchor (track start is always 0)
                return anchorOffset;
            }

            // Clip anchor - resolve recursively
            auto it = clips.find(anchorClipId);
            if (it == clips.end()) {
                // Fallback if anchor not found - treat as track anchor
                return anchorOffset;
            }

            // Recursively resolve anchor chain
            const ClipData* anchorClip = it->second;
            TimelinePosition anchorClipPos = anchorClip->getAbsolutePosition(clips);

            // Calculate anchor point based on origin
            TimelinePosition anchorPoint;
            if (anchorOrigin == AnchorOrigin::Start) {
                anchorPoint = anchorClipPos;  // Start of clip
            } else {
                // End of clip = start + duration
                anchorPoint = anchorClipPos;
                anchorPoint.samples += anchorClip->durationSamples;
            }

            return anchorPoint + anchorOffset;
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
