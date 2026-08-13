#include "uapmd-data/detail/timeline/TimeReferenceResolver.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace uapmd {

    namespace {
        // Resolution is mutually recursive: a marker's position may be
        // expressed relative to another marker. The cache memoises resolved
        // markers, and `resolving` detects cycles by holding the markers
        // currently on the stack.
        using MarkerCache = std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>;
        using MarkerResolving = std::unordered_set<MarkerKey, MarkerKeyHash>;

        std::optional<int64_t> resolveMarkerAbsoluteSample(
            std::string_view ownerReferenceId,
            const ClipMarker& marker,
            const ClipReferenceMap& clipLookup,
            const std::vector<ClipMarker>& masterTrackMarkers,
            double sampleRate,
            MarkerCache& cache,
            MarkerResolving& resolving);

        std::optional<int64_t> resolveReferenceAbsoluteSample(
            std::string_view ownerReferenceId,
            const ClipData* ownerClip,
            const TimeReference& reference,
            const ClipReferenceMap& clipLookup,
            const std::vector<ClipMarker>& masterTrackMarkers,
            double sampleRate,
            MarkerCache& cache,
            MarkerResolving& resolving) {
            switch (reference.type) {
                case TimeReferenceType::ContainerStart: {
                    const std::string effectiveReferenceId = reference.referenceId.empty()
                        ? std::string(ownerReferenceId)
                        : reference.referenceId;
                    if (effectiveReferenceId == kMasterMarkerReferenceId)
                        return 0;
                    if (effectiveReferenceId == ownerReferenceId)
                        return ownerClip ? std::optional<int64_t>(ownerClip->position.samples) : std::optional<int64_t>(0);
                    auto clipIt = clipLookup.find(effectiveReferenceId);
                    return clipIt == clipLookup.end() ? std::nullopt : std::optional<int64_t>(clipIt->second.position.samples);
                }
                case TimeReferenceType::ContainerEnd: {
                    const std::string effectiveReferenceId = reference.referenceId.empty()
                        ? std::string(ownerReferenceId)
                        : reference.referenceId;
                    // The master track has no end to reference.
                    if (effectiveReferenceId == kMasterMarkerReferenceId)
                        return std::nullopt;
                    if (effectiveReferenceId == ownerReferenceId) {
                        if (!ownerClip)
                            return std::nullopt;
                        return ownerClip->position.samples + ownerClip->durationSamples;
                    }
                    auto clipIt = clipLookup.find(effectiveReferenceId);
                    if (clipIt == clipLookup.end())
                        return std::nullopt;
                    return clipIt->second.position.samples + clipIt->second.durationSamples;
                }
                case TimeReferenceType::Point: {
                    std::string containerReferenceId;
                    std::string pointReferenceId;
                    if (!TimeReference::parsePointReferenceId(reference.referenceId, containerReferenceId, pointReferenceId))
                        return std::nullopt;
                    if (containerReferenceId == kMasterMarkerReferenceId) {
                        auto* marker = findMarkerById(masterTrackMarkers, pointReferenceId);
                        if (!marker)
                            return std::nullopt;
                        return resolveMarkerAbsoluteSample(
                            kMasterMarkerReferenceId, *marker, clipLookup, masterTrackMarkers, sampleRate, cache, resolving);
                    }
                    auto clipIt = clipLookup.find(containerReferenceId);
                    if (clipIt == clipLookup.end())
                        return std::nullopt;
                    auto* marker = findMarkerById(clipIt->second.markers, pointReferenceId);
                    if (!marker)
                        return std::nullopt;
                    return resolveMarkerAbsoluteSample(
                        containerReferenceId, *marker, clipLookup, masterTrackMarkers, sampleRate, cache, resolving);
                }
            }

            return std::nullopt;
        }

        std::optional<int64_t> resolveMarkerAbsoluteSample(
            std::string_view ownerReferenceId,
            const ClipMarker& marker,
            const ClipReferenceMap& clipLookup,
            const std::vector<ClipMarker>& masterTrackMarkers,
            double sampleRate,
            MarkerCache& cache,
            MarkerResolving& resolving) {
            MarkerKey key{std::string(ownerReferenceId), marker.markerId};
            if (auto it = cache.find(key); it != cache.end())
                return it->second;

            // Already on the stack, so the references form a cycle.
            if (!resolving.insert(key).second) {
                cache[key] = std::nullopt;
                return std::nullopt;
            }

            const ClipData* ownerClip = nullptr;
            if (ownerReferenceId != kMasterMarkerReferenceId) {
                auto clipIt = clipLookup.find(std::string(ownerReferenceId));
                if (clipIt != clipLookup.end())
                    ownerClip = &clipIt->second;
            }

            auto absoluteReferenceSample = resolveReferenceAbsoluteSample(
                ownerReferenceId,
                ownerClip,
                marker.timeReference(ownerReferenceId, kMasterMarkerReferenceId),
                clipLookup,
                masterTrackMarkers,
                sampleRate,
                cache,
                resolving);

            std::optional<int64_t> resolved;
            if (absoluteReferenceSample)
                resolved = *absoluteReferenceSample + secondsToSamples(marker.clipPositionOffset, sampleRate);

            resolving.erase(key);
            cache[key] = resolved;
            return resolved;
        }

        double safeSampleRate(double sampleRate) {
            return std::max(1.0, sampleRate);
        }
    }

    int64_t secondsToSamples(double seconds, double sampleRate) {
        return static_cast<int64_t>(std::llround(seconds * sampleRate));
    }

    double samplesToSeconds(int64_t samples, double sampleRate) {
        if (sampleRate <= 0.0)
            return 0.0;
        return static_cast<double>(samples) / sampleRate;
    }

    const ClipMarker* findMarkerById(
        const std::vector<ClipMarker>& markers,
        std::string_view markerId) {
        auto it = std::find_if(markers.begin(), markers.end(), [markerId](const auto& marker) {
            return marker.markerId == markerId;
        });
        return it == markers.end() ? nullptr : &(*it);
    }

    std::optional<int64_t> resolveMarkerClipPosition(
        const ClipData& targetClip,
        const ClipMarker& marker,
        const ClipReferenceMap& clipLookup,
        const std::vector<ClipMarker>& masterTrackMarkers,
        double sampleRate) {
        MarkerCache cache;
        MarkerResolving resolving;
        auto absoluteSample = resolveMarkerAbsoluteSample(
            targetClip.referenceId, marker, clipLookup, masterTrackMarkers,
            safeSampleRate(sampleRate), cache, resolving);
        if (!absoluteSample)
            return std::nullopt;
        const int64_t clipPosition = *absoluteSample - targetClip.position.samples;
        if (clipPosition < 0 || clipPosition > targetClip.durationSamples)
            return std::nullopt;
        return clipPosition;
    }

    std::optional<int64_t> resolveAudioWarpClipPosition(
        const ClipData& targetClip,
        const AudioWarpPoint& warp,
        const ClipReferenceMap& clipLookup,
        const std::vector<ClipMarker>& masterTrackMarkers,
        double sampleRate) {
        const double rate = safeSampleRate(sampleRate);
        MarkerCache cache;
        MarkerResolving resolving;
        auto absoluteReferenceSample = resolveReferenceAbsoluteSample(
            targetClip.referenceId,
            &targetClip,
            warp.timeReference(targetClip.referenceId, kMasterMarkerReferenceId),
            clipLookup,
            masterTrackMarkers,
            rate,
            cache,
            resolving);
        if (!absoluteReferenceSample)
            return std::nullopt;
        const int64_t absoluteSample = *absoluteReferenceSample + secondsToSamples(warp.clipPositionOffset, rate);
        const int64_t clipPosition = absoluteSample - targetClip.position.samples;
        if (clipPosition < 0 || clipPosition > targetClip.durationSamples)
            return std::nullopt;
        return clipPosition;
    }

    std::vector<AudioWarpPoint> resolveAudioWarpPoints(
        const ClipData& targetClip,
        const std::vector<AudioWarpPoint>& audioWarps,
        const ClipReferenceMap& clipLookup,
        const std::vector<ClipMarker>& masterTrackMarkers,
        double sampleRate) {
        const double rate = safeSampleRate(sampleRate);
        std::vector<AudioWarpPoint> resolved;
        resolved.reserve(audioWarps.size());
        for (auto warp : audioWarps) {
            if (auto clipPosition = resolveAudioWarpClipPosition(targetClip, warp, clipLookup, masterTrackMarkers, rate)) {
                warp.clipPositionOffset = samplesToSeconds(*clipPosition, rate);
                resolved.push_back(std::move(warp));
            }
        }
        return resolved;
    }

} // namespace uapmd
