#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "TimelineTypes.hpp"

namespace uapmd {

    // Resolution of `TimeReference`s over clips: where a marker or an audio
    // warp point actually falls, given that either may be expressed relative to
    // another clip's start, end or marker.
    //
    // This lives here rather than in the application because it is a property
    // of the document, and because a reference may point at any clip in the
    // project, so resolution needs the whole set rather than one clip.

    // Reference identifier under which the master track and its markers are
    // addressed. It is not a clip, so it has no end and cannot be referenced
    // as a container end.
    inline constexpr std::string_view kMasterMarkerReferenceId = "master_track";

    int64_t secondsToSamples(double seconds, double sampleRate);
    double samplesToSeconds(int64_t samples, double sampleRate);

    const ClipMarker* findMarkerById(
        const std::vector<ClipMarker>& markers,
        std::string_view markerId);

    // A marker is unique only within the object that owns it, so addressing one
    // takes both the owner's reference identifier and the marker's own.
    struct MarkerKey {
        std::string clipReferenceId;
        std::string markerId;

        bool operator==(const MarkerKey& other) const {
            return clipReferenceId == other.clipReferenceId && markerId == other.markerId;
        }
    };

    struct MarkerKeyHash {
        size_t operator()(const MarkerKey& key) const {
            size_t seed = std::hash<std::string>{}(key.clipReferenceId);
            seed ^= std::hash<std::string>{}(key.markerId) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    // Every clip that a reference might point at, addressable by reference
    // identifier.
    using ClipReferenceMap = std::unordered_map<std::string, ClipData>;

    // Position of `marker` within `targetClip`, in samples from the clip's
    // start. Empty when the reference cannot be resolved, forms a cycle, or
    // falls outside the clip.
    std::optional<int64_t> resolveMarkerClipPosition(
        const ClipData& targetClip,
        const ClipMarker& marker,
        const ClipReferenceMap& clipLookup,
        const std::vector<ClipMarker>& masterTrackMarkers,
        double sampleRate);

    // The same for an audio warp point.
    std::optional<int64_t> resolveAudioWarpClipPosition(
        const ClipData& targetClip,
        const AudioWarpPoint& warp,
        const ClipReferenceMap& clipLookup,
        const std::vector<ClipMarker>& masterTrackMarkers,
        double sampleRate);

    // Warp points with their offsets rewritten as clip-relative seconds, which
    // is what an audio source node consumes. Points that cannot be resolved are
    // dropped rather than being given a wrong position.
    std::vector<AudioWarpPoint> resolveAudioWarpPoints(
        const ClipData& targetClip,
        const std::vector<AudioWarpPoint>& audioWarps,
        const ClipReferenceMap& clipLookup,
        const std::vector<ClipMarker>& masterTrackMarkers,
        double sampleRate);

} // namespace uapmd
