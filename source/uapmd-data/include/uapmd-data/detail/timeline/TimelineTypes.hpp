#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
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

    // Shared authored-time reference representation.
    // `referenceId` is empty when the reference is relative to the owning container.
    // `offset` is always expressed in seconds.
    enum class TimeReferenceType {
        ContainerStart,
        ContainerEnd,
        Point
    };

    struct TimeReference {
        TimeReferenceType type{TimeReferenceType::ContainerStart};
        std::string referenceId;
        double offset{0.0};  // seconds

        bool operator==(const TimeReference& other) const {
            return type == other.type &&
                   referenceId == other.referenceId &&
                   offset == other.offset;
        }

        bool isOwningContainerReference() const {
            return referenceId.empty();
        }

        static TimeReference fromContainerStart(std::string referenceId = {}, double offset = 0.0) {
            TimeReference reference;
            reference.type = TimeReferenceType::ContainerStart;
            reference.referenceId = std::move(referenceId);
            reference.offset = offset;
            return reference;
        }

        static TimeReference fromContainerEnd(std::string referenceId = {}, double offset = 0.0) {
            TimeReference reference;
            reference.type = TimeReferenceType::ContainerEnd;
            reference.referenceId = std::move(referenceId);
            reference.offset = offset;
            return reference;
        }

        static TimeReference fromPoint(std::string referenceId, double offset = 0.0) {
            TimeReference reference;
            reference.type = TimeReferenceType::Point;
            reference.referenceId = std::move(referenceId);
            reference.offset = offset;
            return reference;
        }

        static std::string makePointReferenceId(std::string_view containerReferenceId, std::string_view pointReferenceId) {
            std::string encoded = "point:";
            encoded += std::to_string(containerReferenceId.size());
            encoded += ':';
            encoded += containerReferenceId;
            encoded += pointReferenceId;
            return encoded;
        }

        static bool parsePointReferenceId(std::string_view encodedReferenceId,
                                          std::string& containerReferenceId,
                                          std::string& pointReferenceId) {
            constexpr std::string_view prefix = "point:";
            if (!encodedReferenceId.starts_with(prefix))
                return false;

            const auto lengthBegin = prefix.size();
            const auto separator = encodedReferenceId.find(':', lengthBegin);
            if (separator == std::string_view::npos)
                return false;

            const auto lengthText = encodedReferenceId.substr(lengthBegin, separator - lengthBegin);
            if (lengthText.empty())
                return false;

            size_t containerLength = 0;
            for (char ch : lengthText) {
                if (ch < '0' || ch > '9')
                    return false;
                containerLength = containerLength * 10 + static_cast<size_t>(ch - '0');
            }

            const auto containerBegin = separator + 1;
            if (containerBegin + containerLength > encodedReferenceId.size())
                return false;

            containerReferenceId = std::string(encodedReferenceId.substr(containerBegin, containerLength));
            pointReferenceId = std::string(encodedReferenceId.substr(containerBegin + containerLength));
            return !pointReferenceId.empty();
        }
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

        TimeReference timeReference() const {
            switch (referenceType) {
                case AudioWarpReferenceType::Manual:
                case AudioWarpReferenceType::ClipStart:
                    return TimeReference::fromContainerStart(referenceClipId, clipPositionOffset);
                case AudioWarpReferenceType::ClipEnd:
                    return TimeReference::fromContainerEnd(referenceClipId, clipPositionOffset);
                case AudioWarpReferenceType::ClipMarker:
                case AudioWarpReferenceType::MasterMarker:
                    return TimeReference::fromPoint(referenceMarkerId, clipPositionOffset);
            }
            return TimeReference::fromContainerStart(referenceClipId, clipPositionOffset);
        }

        TimeReference timeReference(std::string_view owningContainerReferenceId,
                                    std::string_view masterContainerReferenceId = {}) const {
            switch (referenceType) {
                case AudioWarpReferenceType::Manual:
                case AudioWarpReferenceType::ClipStart:
                    return TimeReference::fromContainerStart(
                        referenceClipId.empty() ? std::string(owningContainerReferenceId) : referenceClipId,
                        clipPositionOffset);
                case AudioWarpReferenceType::ClipEnd:
                    return TimeReference::fromContainerEnd(
                        referenceClipId.empty() ? std::string(owningContainerReferenceId) : referenceClipId,
                        clipPositionOffset);
                case AudioWarpReferenceType::ClipMarker:
                    return TimeReference::fromPoint(
                        TimeReference::makePointReferenceId(
                            referenceClipId.empty() ? owningContainerReferenceId : std::string_view(referenceClipId),
                            referenceMarkerId),
                        clipPositionOffset);
                case AudioWarpReferenceType::MasterMarker:
                    return TimeReference::fromPoint(
                        TimeReference::makePointReferenceId(masterContainerReferenceId, referenceMarkerId),
                        clipPositionOffset);
            }
            return TimeReference::fromContainerStart(std::string(owningContainerReferenceId), clipPositionOffset);
        }

        void setTimeReference(const TimeReference& reference) {
            clipPositionOffset = reference.offset;
            referenceClipId.clear();
            referenceMarkerId.clear();

            switch (reference.type) {
                case TimeReferenceType::ContainerStart:
                    referenceType = AudioWarpReferenceType::ClipStart;
                    referenceClipId = reference.referenceId;
                    break;
                case TimeReferenceType::ContainerEnd:
                    referenceType = AudioWarpReferenceType::ClipEnd;
                    referenceClipId = reference.referenceId;
                    break;
                case TimeReferenceType::Point:
                    referenceType = AudioWarpReferenceType::ClipMarker;
                    referenceMarkerId = reference.referenceId;
                    break;
            }
        }

        void setTimeReference(const TimeReference& reference,
                              std::string_view owningContainerReferenceId,
                              std::string_view masterContainerReferenceId = {}) {
            if (reference.type != TimeReferenceType::Point) {
                setTimeReference(reference);
                if (reference.referenceId == owningContainerReferenceId)
                    referenceClipId.clear();
                return;
            }

            clipPositionOffset = reference.offset;
            referenceClipId.clear();
            referenceMarkerId.clear();

            std::string containerReferenceId;
            if (!TimeReference::parsePointReferenceId(reference.referenceId, containerReferenceId, referenceMarkerId)) {
                referenceType = AudioWarpReferenceType::ClipMarker;
                referenceMarkerId = reference.referenceId;
                return;
            }

            if (!masterContainerReferenceId.empty() && containerReferenceId == masterContainerReferenceId) {
                referenceType = AudioWarpReferenceType::MasterMarker;
                return;
            }

            referenceType = AudioWarpReferenceType::ClipMarker;
            if (containerReferenceId != owningContainerReferenceId)
                referenceClipId = std::move(containerReferenceId);
        }
    };

    struct AudioWarpPoint {
        double clipPositionOffset{0.0};
        double speedRatio{1.0};
        AudioWarpReferenceType referenceType{AudioWarpReferenceType::ClipStart};
        std::string referenceClipId;
        std::string referenceMarkerId;

        TimeReference timeReference() const {
            switch (referenceType) {
                case AudioWarpReferenceType::Manual:
                case AudioWarpReferenceType::ClipStart:
                    return TimeReference::fromContainerStart(referenceClipId, clipPositionOffset);
                case AudioWarpReferenceType::ClipEnd:
                    return TimeReference::fromContainerEnd(referenceClipId, clipPositionOffset);
                case AudioWarpReferenceType::ClipMarker:
                case AudioWarpReferenceType::MasterMarker:
                    return TimeReference::fromPoint(referenceMarkerId, clipPositionOffset);
            }
            return TimeReference::fromContainerStart(referenceClipId, clipPositionOffset);
        }

        TimeReference timeReference(std::string_view owningContainerReferenceId,
                                    std::string_view masterContainerReferenceId = {}) const {
            switch (referenceType) {
                case AudioWarpReferenceType::Manual:
                case AudioWarpReferenceType::ClipStart:
                    return TimeReference::fromContainerStart(
                        referenceClipId.empty() ? std::string(owningContainerReferenceId) : referenceClipId,
                        clipPositionOffset);
                case AudioWarpReferenceType::ClipEnd:
                    return TimeReference::fromContainerEnd(
                        referenceClipId.empty() ? std::string(owningContainerReferenceId) : referenceClipId,
                        clipPositionOffset);
                case AudioWarpReferenceType::ClipMarker:
                    return TimeReference::fromPoint(
                        TimeReference::makePointReferenceId(
                            referenceClipId.empty() ? owningContainerReferenceId : std::string_view(referenceClipId),
                            referenceMarkerId),
                        clipPositionOffset);
                case AudioWarpReferenceType::MasterMarker:
                    return TimeReference::fromPoint(
                        TimeReference::makePointReferenceId(masterContainerReferenceId, referenceMarkerId),
                        clipPositionOffset);
            }
            return TimeReference::fromContainerStart(std::string(owningContainerReferenceId), clipPositionOffset);
        }

        void setTimeReference(const TimeReference& reference) {
            clipPositionOffset = reference.offset;
            referenceClipId.clear();
            referenceMarkerId.clear();

            switch (reference.type) {
                case TimeReferenceType::ContainerStart:
                    referenceType = AudioWarpReferenceType::ClipStart;
                    referenceClipId = reference.referenceId;
                    break;
                case TimeReferenceType::ContainerEnd:
                    referenceType = AudioWarpReferenceType::ClipEnd;
                    referenceClipId = reference.referenceId;
                    break;
                case TimeReferenceType::Point:
                    referenceType = AudioWarpReferenceType::ClipMarker;
                    referenceMarkerId = reference.referenceId;
                    break;
            }
        }

        void setTimeReference(const TimeReference& reference,
                              std::string_view owningContainerReferenceId,
                              std::string_view masterContainerReferenceId = {}) {
            if (reference.type != TimeReferenceType::Point) {
                setTimeReference(reference);
                if (reference.referenceId == owningContainerReferenceId)
                    referenceClipId.clear();
                return;
            }

            clipPositionOffset = reference.offset;
            referenceClipId.clear();
            referenceMarkerId.clear();

            std::string containerReferenceId;
            if (!TimeReference::parsePointReferenceId(reference.referenceId, containerReferenceId, referenceMarkerId)) {
                referenceType = AudioWarpReferenceType::ClipMarker;
                referenceMarkerId = reference.referenceId;
                return;
            }

            if (!masterContainerReferenceId.empty() && containerReferenceId == masterContainerReferenceId) {
                referenceType = AudioWarpReferenceType::MasterMarker;
                return;
            }

            referenceType = AudioWarpReferenceType::ClipMarker;
            if (containerReferenceId != owningContainerReferenceId)
                referenceClipId = std::move(containerReferenceId);
        }
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

        TimeReference timeReference(int32_t sampleRate) const {
            return anchorOrigin == AnchorOrigin::Start
                ? TimeReference::fromContainerStart(anchorReferenceId, anchorOffset.toSeconds(sampleRate))
                : TimeReference::fromContainerEnd(anchorReferenceId, anchorOffset.toSeconds(sampleRate));
        }

        void setTimeReference(const TimeReference& reference, int32_t sampleRate) {
            switch (reference.type) {
                case TimeReferenceType::ContainerStart:
                    anchorOrigin = AnchorOrigin::Start;
                    anchorReferenceId = reference.referenceId;
                    break;
                case TimeReferenceType::ContainerEnd:
                    anchorOrigin = AnchorOrigin::End;
                    anchorReferenceId = reference.referenceId;
                    break;
                case TimeReferenceType::Point:
                    anchorOrigin = AnchorOrigin::Start;
                    anchorReferenceId = reference.referenceId;
                    break;
            }

            anchorOffset = TimelinePosition::fromSeconds(reference.offset, sampleRate);
        }

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
