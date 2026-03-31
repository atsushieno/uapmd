#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace uapmd {

    // Anchor origin - whether the anchor point is at the start or end of a clip/track
    enum class AnchorOrigin {
        Start,  // Anchor at the beginning of the clip/track
        End     // Anchor at the end of the clip/track
    };

    // Represents a position on the timeline
    struct TimelinePosition {
        int64_t samples{0};     // Primary representation (RT-safe)

        // DEPRECATED: Use TrackContext.ppqPosition() instead for musical time
        // This field is kept for backwards compatibility with serialized data only
        double legacy_beats{0.0};

        TimelinePosition() = default;
        explicit TimelinePosition(int64_t s) : samples(s), legacy_beats(0.0) {}

        // Factory methods for creating positions
        static TimelinePosition fromSamples(int64_t samples, int32_t sampleRate, double tempo = 120.0) {
            TimelinePosition pos;
            pos.samples = samples;
            pos.legacy_beats = samplesToBeats(samples, tempo, sampleRate);
            return pos;
        }

        static TimelinePosition fromBeats(double beats, int32_t sampleRate, double tempo = 120.0) {
            TimelinePosition pos;
            pos.legacy_beats = beats;
            pos.samples = beatsToSamples(beats, tempo, sampleRate);
            return pos;
        }

        static TimelinePosition fromSeconds(double seconds, int32_t sampleRate, double tempo = 120.0) {
            TimelinePosition pos;
            pos.samples = static_cast<int64_t>(seconds * sampleRate);
            pos.legacy_beats = samplesToBeats(pos.samples, tempo, sampleRate);
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
            result.legacy_beats = legacy_beats + other.legacy_beats;
            return result;
        }

        TimelinePosition operator-(const TimelinePosition& other) const {
            TimelinePosition result;
            result.samples = samples - other.samples;
            result.legacy_beats = legacy_beats - other.legacy_beats;
            return result;
        }
    };

    // Clip type discrimination
    enum class ClipType {
        Audio,  // filepath points to audio file (WAV/FLAC/OGG)
        Midi    // filepath points to MIDI file (SMF/SMF2)
    };

    enum class AudioWarpReferenceType {
        Manual,
        ClipStart,
        ClipEnd,
        ClipMarker,
        MasterMarker
    };

    struct ClipMarker {
        std::string markerId;
        double clipPositionOffset{0.0};
        AudioWarpReferenceType referenceType{AudioWarpReferenceType::ClipStart};
        std::string referenceClipId;
        std::string referenceMarkerId;
        std::string name;
    };

    struct AudioWarpPoint {
        double clipPositionOffset{0.0};
        double speedRatio{1.0};
        AudioWarpReferenceType referenceType{AudioWarpReferenceType::ClipStart};
        std::string referenceClipId;
        std::string referenceMarkerId;
    };

    // Represents a single clip on a track
    struct ClipData {
        int32_t clipId{-1};
        std::string referenceId;
        TimelinePosition position;          // Absolute position on timeline (calculated from anchor)
        int64_t durationSamples{0};         // Duration of clip (full file length)
        int32_t sourceNodeInstanceId{-1};  // Which source node plays this clip

        // Playback properties
        double gain{1.0};
        bool muted{false};
        std::string name;                   // User-editable clip name
        std::string filepath;               // Source audio file path
        bool needsFileSave{false};          // True when the source file must be copied into the project

        // Clip type and MIDI-specific fields
        ClipType clipType{ClipType::Audio};
        uint32_t tickResolution{480};       // Ticks per quarter note from SMF (only valid for MIDI clips)
        double clipTempo{120.0};            // Original tempo for time-stretching (only valid for MIDI clips)
        // When true, MIDI2 Assignable Controller (NRPN) messages in this clip are intercepted
        // at playback time and remapped to plugin parameter changes instead of being forwarded
        // as raw UMP to the plugin.  Plugins that consume NRPNs natively should leave this false.
        bool nrpnToParameterMapping{false};

        // Clip markers shared by audio and MIDI clips.
        std::vector<ClipMarker> markers;
        // Audio-only warp anchors. Each point defines the source anchor and speed
        // ratio starting at the resolved clip-local position until the next point.
        std::vector<AudioWarpPoint> audioWarps;

        // Anchor support (NEW)
        std::string anchorReferenceId;     // Empty = track anchor (absolute position)
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
        int64_t getSourcePosition(const TimelinePosition& timelinePos, const std::unordered_map<std::string, const ClipData*>& clips) const {
            TimelinePosition absPos = getAbsolutePosition(clips);
            if (timelinePos.samples < absPos.samples ||
                timelinePos.samples >= absPos.samples + durationSamples)
                return -1;
            return timelinePos.samples - absPos.samples;
        }

        // Helper: Calculate absolute position from anchor
        // For RT-safe queries, use TrackClipManager's cached resolution
        TimelinePosition getAbsolutePosition(const std::unordered_map<std::string, const ClipData*>& clips) const {
            if (anchorReferenceId.empty()) {
                // Track anchor - anchorOffset is absolute position
                // anchorOrigin is meaningless for track anchor (track start is always 0)
                return anchorOffset;
            }

            // Clip anchor - resolve recursively
            auto it = clips.find(anchorReferenceId);
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
        int32_t sample_rate{48000};             // Added for convenience

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

            // Update legacy_beats representation
            playheadPosition.legacy_beats = TimelinePosition::samplesToBeats(
                playheadPosition.samples, tempo, sampleRate
            );
        }

        // Seek to a specific position
        void seekTo(const TimelinePosition& position, int32_t sampleRate) {
            playheadPosition = position;
            playheadPosition.legacy_beats = TimelinePosition::samplesToBeats(
                playheadPosition.samples, tempo, sampleRate
            );
        }
    };

} // namespace uapmd
