#include <cctype>
#include <cstring>
#include <bit>
#include <algorithm>
#include <map>
#include <set>
#include <format>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <limits>
#include "PlatformDialogs.hpp"

#include <imgui.h>
#include <umppi/umppi.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-data/detail/timeline/MidiClipSourceNode.hpp"

#include "TimelineEditor.hpp"
#include "BeatsTimelineConstants.hpp"
#include "ClipPreview.hpp"
#include <uapmd-app-model/uapmd-app-model.hpp>
#include "ContextActions.hpp"
#include "FontIcons.hpp"

namespace uapmd::gui {

namespace {
constexpr int32_t kMasterTrackClipId = -1000;
constexpr double kDisplayDefaultBpm = 120.0;
constexpr float kTrackGainUiMinDb = -60.0f;
constexpr float kTrackGainUiMaxDb = 18.0618f;
// Slider position where 0 dB (unity gain) sits — 70% from left.
constexpr float kSliderUnityPos = 0.7f;
// Exponent for the sub-unity power curve: > 1 compresses the far-left extreme
// and expands the region near 0 dB so the practical mixing range feels wider.
constexpr float kSliderCurve = 2.0f;

struct ClipKey {
    std::string referenceId;

    bool operator==(const ClipKey& other) const {
        return referenceId == other.referenceId;
    }
};

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

struct ClipKeyHash {
    size_t operator()(const ClipKey& key) const {
        return std::hash<std::string>{}(key.referenceId);
    }
};

bool renderIconButtonWithTooltip(const char* label, const char* tooltip) {
    return contextActionButton(label, ImVec2(0.0f, 0.0f), tooltip);
}

float linearGainToSliderDb(double gain) {
    if (gain <= 0.0)
        return kTrackGainUiMinDb;
    return std::clamp(
        static_cast<float>(20.0 * std::log10(gain)),
        kTrackGainUiMinDb,
        kTrackGainUiMaxDb);
}

double sliderDbToLinearGain(float db) {
    if (db <= kTrackGainUiMinDb)
        return 0.0;
    return std::clamp(
        static_cast<double>(std::pow(10.0f, db / 20.0f)),
        0.0,
        8.0);
}

// Maps a dB value to a slider position in [0, 1].
// Below 0 dB a power curve (exponent kSliderCurve) expands the region near
// unity; above 0 dB the mapping is linear.  0 dB always lands at kSliderUnityPos.
float dbToSliderPos(float db) {
    if (db <= kTrackGainUiMinDb) return 0.0f;
    if (db >= kTrackGainUiMaxDb) return 1.0f;
    if (db <= 0.0f) {
        const float norm = (db - kTrackGainUiMinDb) / (0.0f - kTrackGainUiMinDb);
        return kSliderUnityPos * std::pow(norm, kSliderCurve);
    }
    return kSliderUnityPos + (1.0f - kSliderUnityPos) * (db / kTrackGainUiMaxDb);
}

// Inverse of dbToSliderPos.
float sliderPosToDb(float t) {
    if (t <= 0.0f) return kTrackGainUiMinDb;
    if (t >= 1.0f) return kTrackGainUiMaxDb;
    if (t <= kSliderUnityPos) {
        const float norm = std::pow(t / kSliderUnityPos, 1.0f / kSliderCurve);
        return kTrackGainUiMinDb + norm * (0.0f - kTrackGainUiMinDb);
    }
    return kTrackGainUiMaxDb * (t - kSliderUnityPos) / (1.0f - kSliderUnityPos);
}

uint64_t mixHash(uint64_t seed, uint64_t value) {
    constexpr uint64_t kPrime = 1099511628211ull;
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    seed *= kPrime;
    return seed;
}

uint64_t midiSourceFingerprint(const uapmd::MidiClipSourceNode& midiSource) {
    const auto& words = midiSource.umpEvents();
    const auto& ticks = midiSource.eventTimestampsTicks();

    uint64_t hash = 1469598103934665603ull;
    hash = mixHash(hash, words.size());
    hash = mixHash(hash, ticks.size());
    hash = mixHash(hash, static_cast<uint64_t>(midiSource.tickResolution()));
    hash = mixHash(hash, static_cast<uint64_t>(midiSource.clipTempo() * 1000.0));

    const size_t sampleCount = std::min<size_t>(words.size(), 24);
    for (size_t i = 0; i < sampleCount; ++i) {
        size_t index = (sampleCount <= 1 || words.size() <= 1)
            ? 0
            : (i * (words.size() - 1)) / (sampleCount - 1);
        hash = mixHash(hash, words[index]);
        if (index < ticks.size())
            hash = mixHash(hash, ticks[index]);
    }

    return hash;
}

int64_t secondsToSamples(double seconds, double sampleRate) {
    return static_cast<int64_t>(std::llround(seconds * sampleRate));
}

double samplesToSeconds(int64_t samples, double sampleRate) {
    if (sampleRate <= 0.0)
        return 0.0;
    return static_cast<double>(samples) / sampleRate;
}

bool referencesThisClipEnd(const uapmd::ClipMarker& marker, std::string_view clipReferenceId) {
    if (marker.referenceType != uapmd::AudioWarpReferenceType::ClipEnd)
        return false;
    return marker.referenceClipId.empty() || marker.referenceClipId == clipReferenceId;
}

bool referencesThisClipEnd(const uapmd::AudioWarpPoint& warp, std::string_view clipReferenceId) {
    if (warp.referenceType != uapmd::AudioWarpReferenceType::ClipEnd)
        return false;
    return warp.referenceClipId.empty() || warp.referenceClipId == clipReferenceId;
}

bool wouldCreateClipAnchorCycle(
    const std::vector<uapmd::TimelineTrack*>& tracks,
    uapmd::TimelineTrack* masterTrack,
    int32_t targetTrackIndex,
    int32_t targetClipId,
    std::string_view newAnchorReferenceId
) {
    if (newAnchorReferenceId.empty())
        return false;

    std::unordered_map<std::string, uapmd::ClipData> clipsByReferenceId;
    clipsByReferenceId.reserve(128);
    std::string targetReferenceId;

    auto collectTrack = [&](uapmd::TimelineTrack* track, int32_t trackIndex) {
        if (!track)
            return;
        for (const auto& clip : track->clipManager().getAllClips()) {
            clipsByReferenceId.emplace(clip.referenceId, clip);
            if (trackIndex == targetTrackIndex && clip.clipId == targetClipId)
                targetReferenceId = clip.referenceId;
        }
    };

    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i)
        collectTrack(tracks[i], i);
    collectTrack(masterTrack, uapmd::kMasterTrackIndex);

    if (targetReferenceId.empty())
        return false;
    if (targetReferenceId == newAnchorReferenceId)
        return true;

    std::unordered_set<std::string> visited;
    std::string currentReferenceId(newAnchorReferenceId);
    while (!currentReferenceId.empty()) {
        if (!visited.insert(currentReferenceId).second)
            return true;
        if (currentReferenceId == targetReferenceId)
            return true;

        auto it = clipsByReferenceId.find(currentReferenceId);
        if (it == clipsByReferenceId.end())
            return false;

        currentReferenceId = it->second.timeReference(uapmd::AppModel::instance().sampleRate()).referenceId;
    }
    return false;
}

int32_t toTimelineFrame(double units) {
    if (!std::isfinite(units))
        return 0;
    const double maxUnits = static_cast<double>(std::numeric_limits<int32_t>::max() - 1);
    const double clamped = std::clamp(units, 0.0, maxUnits);
    return static_cast<int32_t>(std::llround(clamped));
}

std::vector<MidiDumpWindow::EventRow> buildMidiDumpRows(
    const uapmd::MidiClipReader::ClipInfo& clipInfo,
    uint32_t tickResolution,
    double tempo
) {
    std::vector<MidiDumpWindow::EventRow> rows;
    if (clipInfo.ump_data.empty())
        return rows;

    const double safeResolution = tickResolution > 0 ? static_cast<double>(tickResolution) : 1.0;
    const double safeTempo = tempo > 0.0 ? tempo : 120.0;

    size_t index = 0;
    rows.reserve(clipInfo.ump_data.size());
    while (index < clipInfo.ump_data.size()) {
        const uint32_t word = clipInfo.ump_data[index];
        const size_t byteLength = umppi::Ump{word}.getSizeInBytes();
        size_t wordCount = (byteLength + sizeof(uint32_t) - 1) / sizeof(uint32_t);
        wordCount = std::max<size_t>(1, wordCount);
        const size_t availableWords = clipInfo.ump_data.size() - index;
        if (wordCount > availableWords)
            wordCount = availableWords;

        double ticks = 0.0;
        if (index < clipInfo.ump_tick_timestamps.size())
            ticks = static_cast<double>(clipInfo.ump_tick_timestamps[index]);
        const double beats = ticks / safeResolution;
        const double seconds = beats * (60.0 / safeTempo);

        MidiDumpWindow::EventRow row;
        row.timeSeconds = seconds;
        row.tickPosition = static_cast<uint64_t>(ticks);
        const uint64_t prevTick = rows.empty() ? 0 : rows.back().tickPosition;
        row.deltaTicks = row.tickPosition > prevTick ? row.tickPosition - prevTick : 0;
        row.timeLabel = std::format("{:.3f}s [{}]", seconds, row.tickPosition);

        bool firstByte = true;
        for (size_t offset = 0; offset < wordCount; ++offset) {
            const uint32_t dataWord = clipInfo.ump_data[index + offset];
            for (int shift = 24; shift >= 0; shift -= 8) {
                const uint8_t byteValue = static_cast<uint8_t>((dataWord >> shift) & 0xFF);
                if (!firstByte)
                    row.hexBytes.push_back(' ');
                firstByte = false;
                row.hexBytes += std::format("{:02X}", byteValue);
            }
        }

        rows.push_back(std::move(row));
        index += wordCount;
    }

    return rows;
}

bool hasExtension(std::string_view ext, std::initializer_list<std::string_view> expected) {
    for (auto candidate : expected) {
        if (ext == candidate)
            return true;
    }
    return false;
}

std::string lowerCaseExtension(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool looksLikeMidiFile(const std::string& path, std::string_view ext) {
    if (hasExtension(ext, {".mid", ".midi", ".smf", ".midi2"}))
        return true;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    char header[4]{};
    file.read(header, sizeof(header));
    return file.gcount() == 4 && std::memcmp(header, "MThd", 4) == 0;
}

bool looksLikeAudioFile(std::string_view ext) {
    return hasExtension(ext, {".wav", ".flac", ".ogg"});
}

std::unordered_map<std::string, uapmd::ClipData> buildClipReferenceMap(const std::vector<uapmd::TimelineTrack*>& tracks) {
    std::unordered_map<std::string, uapmd::ClipData> clipMap;
    for (auto* track : tracks) {
        if (!track)
            continue;
        auto clips = track->clipManager().getAllClips();
        for (auto& clip : clips)
            clipMap[clip.referenceId] = std::move(clip);
    }
    return clipMap;
}

constexpr std::string_view kMasterMarkerReferenceId = "master_track";

const uapmd::ClipMarker* findMarkerById(const std::vector<uapmd::ClipMarker>& markers, std::string_view markerId) {
    auto it = std::find_if(markers.begin(), markers.end(), [markerId](const auto& marker) {
        return marker.markerId == markerId;
    });
    return it == markers.end() ? nullptr : &(*it);
}

std::optional<int64_t> resolveMarkerAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
);

std::optional<int64_t> resolveReferenceAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipData* ownerClip,
    const uapmd::TimeReference& reference,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
) {
    switch (reference.type) {
        case uapmd::TimeReferenceType::ContainerStart: {
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
        case uapmd::TimeReferenceType::ContainerEnd: {
            const std::string effectiveReferenceId = reference.referenceId.empty()
                ? std::string(ownerReferenceId)
                : reference.referenceId;
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
        case uapmd::TimeReferenceType::Point: {
            std::string containerReferenceId;
            std::string pointReferenceId;
            if (!uapmd::TimeReference::parsePointReferenceId(reference.referenceId, containerReferenceId, pointReferenceId))
                return std::nullopt;
            if (containerReferenceId == kMasterMarkerReferenceId) {
                auto* marker = findMarkerById(masterTrackMarkers, pointReferenceId);
                if (!marker)
                    return std::nullopt;
                return resolveMarkerAbsoluteSample(kMasterMarkerReferenceId, *marker, clipLookup, masterTrackMarkers, cache, resolving);
            }
            auto clipIt = clipLookup.find(containerReferenceId);
            if (clipIt == clipLookup.end())
                return std::nullopt;
            auto* marker = findMarkerById(clipIt->second.markers, pointReferenceId);
            if (!marker)
                return std::nullopt;
            return resolveMarkerAbsoluteSample(containerReferenceId, *marker, clipLookup, masterTrackMarkers, cache, resolving);
        }
    }

    return std::nullopt;
}

std::optional<int64_t> resolveMarkerAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
) {
    MarkerKey key{std::string(ownerReferenceId), marker.markerId};
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    if (!resolving.insert(key).second) {
        cache[key] = std::nullopt;
        return std::nullopt;
    }

    const uapmd::ClipData* ownerClip = nullptr;
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
        cache,
        resolving);

    std::optional<int64_t> resolved;
    if (absoluteReferenceSample) {
        const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
        resolved = *absoluteReferenceSample + secondsToSamples(marker.clipPositionOffset, sampleRate);
    }

    resolving.erase(key);
    cache[key] = resolved;
    return resolved;
}

std::optional<int64_t> resolveMarkerClipPosition(
    const uapmd::ClipData& targetClip,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash> cache;
    std::unordered_set<MarkerKey, MarkerKeyHash> resolving;
    auto absoluteSample = resolveMarkerAbsoluteSample(targetClip.referenceId, marker, clipLookup, masterTrackMarkers, cache, resolving);
    if (!absoluteSample)
        return std::nullopt;
    const int64_t clipPosition = *absoluteSample - targetClip.position.samples;
    if (clipPosition < 0 || clipPosition > targetClip.durationSamples)
        return std::nullopt;
    return clipPosition;
}

std::optional<int64_t> resolveAudioWarpClipPosition(
    const uapmd::ClipData& targetClip,
    const uapmd::AudioWarpPoint& warp,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash> cache;
    std::unordered_set<MarkerKey, MarkerKeyHash> resolving;
    auto absoluteReferenceSample = resolveReferenceAbsoluteSample(
        targetClip.referenceId,
        &targetClip,
        warp.timeReference(targetClip.referenceId, kMasterMarkerReferenceId),
        clipLookup,
        masterTrackMarkers,
        cache,
        resolving);
    if (!absoluteReferenceSample)
        return std::nullopt;
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    const int64_t absoluteSample = *absoluteReferenceSample + secondsToSamples(warp.clipPositionOffset, sampleRate);
    const int64_t clipPosition = absoluteSample - targetClip.position.samples;
    if (clipPosition < 0 || clipPosition > targetClip.durationSamples)
        return std::nullopt;
    return clipPosition;
}

std::vector<uapmd::AudioWarpPoint> resolveAudioWarpPoints(
    const uapmd::ClipData& targetClip,
    const std::vector<uapmd::AudioWarpPoint>& audioWarps,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    std::vector<uapmd::AudioWarpPoint> resolved;
    resolved.reserve(audioWarps.size());
    for (auto warp : audioWarps) {
        if (auto clipPosition = resolveAudioWarpClipPosition(targetClip, warp, clipLookup, masterTrackMarkers)) {
            warp.clipPositionOffset = samplesToSeconds(*clipPosition, sampleRate);
            resolved.push_back(std::move(warp));
        }
    }
    return resolved;
}

std::string clipDisplayName(const uapmd::ClipData& clip) {
    return clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
}

std::string markerDisplayName(const uapmd::ClipMarker& marker, size_t index) {
    if (!marker.name.empty())
        return marker.name;
    if (!marker.markerId.empty())
        return marker.markerId;
    return std::format("Marker {}", index + 1);
}

void appendReferenceOptionsForClip(
    std::vector<AudioEventListEditor::ClipData::ReferencePointOption>& options,
    const uapmd::ClipData& targetClip,
    const uapmd::ClipData& sourceClip,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    std::string_view prefix
) {
    auto makeWarp = [&](uapmd::AudioWarpReferenceType type, std::string markerId = {}) {
        uapmd::AudioWarpPoint warp;
        warp.referenceType = type;
        warp.referenceClipId = sourceClip.referenceId;
        warp.referenceMarkerId = std::move(markerId);
        return warp;
    };

    for (const auto type : {uapmd::AudioWarpReferenceType::ClipStart, uapmd::AudioWarpReferenceType::ClipEnd}) {
        AudioEventListEditor::ClipData::ReferencePointOption option;
        option.referenceType = type;
        option.referenceClipId = sourceClip.referenceId;
        option.label = std::format("{} {}", prefix, type == uapmd::AudioWarpReferenceType::ClipStart ? "Start" : "End");
        if (auto clipPosition = resolveAudioWarpClipPosition(targetClip, makeWarp(type), clipLookup, {})) {
            option.clipPositionSamples = *clipPosition;
            option.resolved = true;
        }
        if (!option.resolved)
            option.label += " (out of range)";
        options.push_back(std::move(option));
    }

    for (size_t markerIndex = 0; markerIndex < sourceClip.markers.size(); ++markerIndex) {
        const auto& marker = sourceClip.markers[markerIndex];
        AudioEventListEditor::ClipData::ReferencePointOption option;
        option.referenceType = uapmd::AudioWarpReferenceType::ClipMarker;
        option.referenceClipId = sourceClip.referenceId;
        option.referenceMarkerId = marker.markerId;
        option.label = std::format("{} Marker {}", prefix, markerDisplayName(marker, markerIndex));
        uapmd::AudioWarpPoint warp;
        warp.referenceType = option.referenceType;
        warp.referenceClipId = option.referenceClipId;
        warp.referenceMarkerId = option.referenceMarkerId;
        if (auto clipPosition = resolveAudioWarpClipPosition(targetClip, warp, clipLookup, {})) {
            option.clipPositionSamples = *clipPosition;
            option.resolved = true;
        }
        if (!option.resolved)
            option.label += " (out of range)";
        options.push_back(std::move(option));
    }
}

std::vector<AudioEventListEditor::ClipData::ReferencePointOption> buildExternalReferenceOptions(
    int32_t targetTrackIndex,
    int32_t targetClipId,
    const AudioEventListEditor& editor,
    const std::vector<uapmd::TimelineTrack*>& tracks,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::vector<AudioEventListEditor::ClipData::ReferencePointOption> options;
    if (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int32_t>(tracks.size()))
        return options;

    auto* targetTrack = tracks[targetTrackIndex];
    if (!targetTrack)
        return options;

    auto* targetClip = targetTrack->clipManager().getClip(targetClipId);
    if (!targetClip)
        return options;

    const auto draftMarkers = editor.draftMarkersByClipReference();
    const auto draftMasterMarkers = editor.draftMasterMarkers();
    auto clipLookup = buildClipReferenceMap(tracks);

    for (auto& [referenceId, clipData] : clipLookup) {
        auto draftIt = draftMarkers.find(referenceId);
        if (draftIt != draftMarkers.end())
            clipData.markers = draftIt->second;
    }

    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
        auto* sourceTrack = tracks[i];
        if (!sourceTrack)
            continue;
        for (auto otherClip : sourceTrack->clipManager().getAllClips()) {
            if (i == targetTrackIndex && otherClip.clipId == targetClipId)
                continue;
            auto draftIt = draftMarkers.find(otherClip.referenceId);
            if (draftIt != draftMarkers.end())
                otherClip.markers = draftIt->second;
            const std::string prefix = std::format("Track {} / {}", i + 1, clipDisplayName(otherClip));
            appendReferenceOptionsForClip(options, *targetClip, otherClip, clipLookup, prefix);
        }
    }

    const auto& masterMarkers = draftMasterMarkers.empty() ? masterTrackMarkers : draftMasterMarkers;
    for (size_t i = 0; i < masterMarkers.size(); ++i) {
        const auto& marker = masterMarkers[i];
        AudioEventListEditor::ClipData::ReferencePointOption option;
        option.referenceType = uapmd::AudioWarpReferenceType::MasterMarker;
        option.referenceMarkerId = marker.markerId;
        option.label = std::format("Master Track Marker {}", markerDisplayName(marker, i));
        uapmd::AudioWarpPoint warp;
        warp.referenceType = option.referenceType;
        warp.referenceMarkerId = option.referenceMarkerId;
        if (auto clipPosition = resolveAudioWarpClipPosition(*targetClip, warp, clipLookup, masterMarkers)) {
            option.clipPositionSamples = *clipPosition;
            option.resolved = true;
        }
        if (!option.resolved)
            option.label += " (out of range)";
        options.push_back(std::move(option));
    }

    return options;
}

bool validateMarkerReferenceSelection(
    int32_t targetTrackIndex,
    int32_t targetClipId,
    std::string_view markerId,
    uapmd::AudioWarpReferenceType referenceType,
    std::string_view referenceClipId,
    std::string_view referenceMarkerId,
    const AudioEventListEditor& editor,
    const std::vector<uapmd::TimelineTrack*>& tracks,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    constexpr std::string_view kMasterRef = "master_track";
    if (referenceType != uapmd::AudioWarpReferenceType::ClipMarker &&
        referenceType != uapmd::AudioWarpReferenceType::MasterMarker) {
        return true;
    }

    const auto clipLookup = buildClipReferenceMap(tracks);
    const auto draftMarkers = editor.draftMarkersByClipReference();
    const auto draftMasterMarkers = editor.draftMasterMarkers();

    std::unordered_map<std::string, std::vector<uapmd::ClipMarker>> markersByOwner;
    for (const auto& [referenceId, clipData] : clipLookup) {
        if (auto it = draftMarkers.find(referenceId); it != draftMarkers.end())
            markersByOwner[referenceId] = it->second;
        else
            markersByOwner[referenceId] = clipData.markers;
    }
    markersByOwner[std::string(kMasterRef)] = draftMasterMarkers.empty() ? masterTrackMarkers : draftMasterMarkers;

    std::string targetOwnerRef = std::string(kMasterRef);
    if (targetTrackIndex != uapmd::kMasterTrackIndex) {
        if (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[targetTrackIndex])
            return true;
        auto* targetClip = tracks[targetTrackIndex]->clipManager().getClip(targetClipId);
        if (!targetClip)
            return true;
        targetOwnerRef = targetClip->referenceId;
    }

    MarkerKey targetKey{targetOwnerRef, std::string(markerId)};
    MarkerKey startKey{
        referenceType == uapmd::AudioWarpReferenceType::MasterMarker
            ? std::string(kMasterRef)
            : (referenceClipId.empty() ? targetOwnerRef : std::string(referenceClipId)),
        std::string(referenceMarkerId)
    };
    if (startKey == targetKey)
        return false;

    auto findMarker = [&](const MarkerKey& key) -> const uapmd::ClipMarker* {
        auto ownerIt = markersByOwner.find(key.clipReferenceId);
        if (ownerIt == markersByOwner.end())
            return nullptr;
        return findMarkerById(ownerIt->second, key.markerId);
    };

    std::unordered_set<MarkerKey, MarkerKeyHash> visited;
    std::function<bool(const MarkerKey&)> reachesTarget = [&](const MarkerKey& key) -> bool {
        if (!visited.insert(key).second)
            return false;
        if (key == targetKey)
            return true;
        const auto* marker = findMarker(key);
        if (!marker)
            return false;
        MarkerKey nextKey;
        switch (marker->referenceType) {
            case uapmd::AudioWarpReferenceType::ClipMarker:
                nextKey.clipReferenceId = marker->referenceClipId.empty() ? key.clipReferenceId : marker->referenceClipId;
                nextKey.markerId = marker->referenceMarkerId;
                if (nextKey.markerId.empty())
                    return false;
                return reachesTarget(nextKey);
            case uapmd::AudioWarpReferenceType::MasterMarker:
                nextKey.clipReferenceId = std::string(kMasterRef);
                nextKey.markerId = marker->referenceMarkerId;
                if (nextKey.markerId.empty())
                    return false;
                return reachesTarget(nextKey);
            default:
                return false;
        }
    };

    return !reachesTarget(startKey);
}
}  // namespace

TimelineEditor::TimelineEditor() {
    // Set up PluginSelector callbacks
    pluginSelector_.setOnInstantiatePlugin([this](const std::string& format, const std::string& pluginId, int32_t trackIndex) {
        // Create plugin instance through AppModel
        auto& appModel = uapmd::AppModel::instance();
        uapmd::AppModel::PluginInstanceConfig config;
        appModel.createPluginInstanceAsync(format, pluginId, trackIndex, config);
        showPluginSelectorWindow_ = false;
    });

    pluginSelector_.setOnScanPlugins([](bool forceRescan, bool useRemoteProcess, double remoteTimeoutSeconds) {
        uapmd::AppModel::instance().performPluginScanning(
            forceRescan,
            useRemoteProcess
                ? uapmd::AppModel::PluginScanRequest::RemoteProcess
                : uapmd::AppModel::PluginScanRequest::InProcess,
            remoteTimeoutSeconds);
    });

    refreshAllSequenceEditorTracks();
}

void TimelineEditor::setCallbacks(TimelineEditorCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void TimelineEditor::setChildWindowSizeHelper(
    std::function<void(const std::string&, ImVec2)> setSize,
    std::function<void(const std::string&)> updateSize
) {
    setNextChildWindowSize_ = std::move(setSize);
    updateChildWindowSizeState_ = std::move(updateSize);
}

void TimelineEditor::update() {
    // Currently empty - reserved for future frame updates
}

SequenceEditor::RenderContext TimelineEditor::buildRenderContext(float uiScale) {
    // Row 1: Clips + Graph + Slider(1.5 slots) + Bypass = 4.5 icon slots, 3 gaps + left/right pad
    const float pad = 4.0f * uiScale;
    const float framePadX = ImGui::GetStyle().FramePadding.x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float iconBtnW = std::max({
        ImGui::CalcTextSize(icons::Clips).x,
        ImGui::CalcTextSize(icons::Graph).x,
        ImGui::CalcTextSize(icons::ToggleOn).x,
        ImGui::CalcTextSize(icons::DeleteTrack).x,
    }) + framePadX * 2.0f;
    const float row1W = pad + 4.5f * iconBtnW + 3.0f * gap + pad;
    // Row 2 must fit the master label, or the regular label plus its delete button.
    const std::string masterPluginLabel = std::format("{} Add Master Plugin", icons::ContextMenu);
    const std::string trackPluginLabel = std::format("{} Add Plugin", icons::ContextMenu);
    const float masterPluginWidth = ImGui::CalcTextSize(masterPluginLabel.c_str()).x + framePadX * 2.0f;
    const float trackPluginWidth = ImGui::CalcTextSize(trackPluginLabel.c_str()).x + framePadX * 2.0f;
    const float masterRow2W = pad + masterPluginWidth + pad;
    const float freezePolicyWidth = ImGui::CalcTextSize("Auto").x + framePadX * 2.0f;
    const float trackRow2W = pad + freezePolicyWidth + 2.0f * gap + trackPluginWidth + gap + iconBtnW + pad;
    const float legendWidth = std::max({row1W, masterRow2W, trackRow2W});

    return SequenceEditor::RenderContext{
        .refreshClips = [this](int32_t trackIndex) {
            refreshSequenceEditorForTrack(trackIndex);
        },
        .addClip = [this](int32_t trackIndex, const std::string& filepath) {
            addClipToTrack(trackIndex, filepath);
        },
        .addClipAtPosition = [this](int32_t trackIndex, const std::string& filepath, double positionSeconds) {
            addClipToTrackAtPosition(trackIndex, filepath, positionSeconds);
        },
        .addAudioClip = [this](int32_t trackIndex, double positionSeconds) {
            addAudioClipToTrack(trackIndex, positionSeconds);
        },
        .addSmfClip = [this](int32_t trackIndex, double positionSeconds) {
            addSmfClipToTrack(trackIndex, positionSeconds);
        },
        .addSmf2Clip = [this](int32_t trackIndex) {
            addSmf2ClipToTrack(trackIndex);
        },
        .addBlankMidiClipAtPosition = [this](int32_t trackIndex, double positionSeconds) {
            addBlankMidi2ClipToTrackAtPosition(trackIndex, positionSeconds);
        },
        .addBlankMidiClipInRange = [this](int32_t trackIndex, double startSeconds, double endSeconds) {
            addBlankMidiClipInRange(trackIndex, startSeconds, endSeconds);
        },
        .addEmptyAudioClipInRange = [this](int32_t trackIndex, double startSeconds, double endSeconds) {
            addEmptyAudioClipInRange(trackIndex, startSeconds, endSeconds);
        },
        .removeClip = [this](int32_t trackIndex, int32_t clipId) {
            removeClipFromTrack(trackIndex, clipId);
        },
        .clearAllClips = [this](int32_t trackIndex) {
            clearAllClipsFromTrack(trackIndex);
        },
        .updateClip = [this](int32_t trackIndex, int32_t clipId, const std::string& anchorReferenceId, const std::string& origin, const std::string& position) {
            updateClip(trackIndex, clipId, anchorReferenceId, origin, position);
        },
        .updateClipName = [this](int32_t trackIndex, int32_t clipId, const std::string& name) {
            updateClipName(trackIndex, clipId, name);
        },
        .changeClipFile = [this](int32_t trackIndex, int32_t clipId) {
            changeClipFile(trackIndex, clipId);
        },
        .moveClipAbsolute = [this](int32_t trackIndex, int32_t clipId, double seconds) {
            moveClipAbsolute(trackIndex, clipId, seconds);
        },
        .showMidiClipDump = [this](int32_t trackIndex, int32_t clipId) {
            showMidiClipDump(trackIndex, clipId);
        },
        .showAudioClipEvents = [this](int32_t trackIndex, int32_t clipId) {
            showAudioClipEvents(trackIndex, clipId);
        },
        .showPianoRoll = [this](int32_t trackIndex, int32_t clipId) {
            showPianoRoll(trackIndex, clipId);
        },
        .showMasterTrackDump = [this]() {
            showMasterMetaDump();
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .renderLegendContent = [this](int32_t trackIndex, const ImRect& legendArea) {
            renderTrackLegendContent(trackIndex, legendArea);
        },
        .timelineUnitsLabel = timelineUnitsLabel_.c_str(),
        .uiScale = uiScale,
        .legendWidth = legendWidth,
    };
}

BeatsSequenceEditor::RenderContext TimelineEditor::buildBeatsRenderContext(float uiScale, float legendWidth) {
    return BeatsSequenceEditor::RenderContext{
        .refreshClips = [this](int32_t trackIndex) {
            refreshBeatsSequenceEditorForTrack(trackIndex);
        },
        .addBlankMidiClipAtPosition = [this](int32_t trackIndex, double positionSeconds) {
            addBlankMidi2ClipToTrackAtPosition(trackIndex, positionSeconds);
        },
        .addAudioClip = [this](int32_t trackIndex, double positionSeconds) {
            addAudioClipToTrack(trackIndex, positionSeconds);
        },
        .addSmfClip = [this](int32_t trackIndex, double positionSeconds) {
            addSmfClipToTrack(trackIndex, positionSeconds);
        },
        .addSmf2Clip = [this](int32_t trackIndex) {
            addSmf2ClipToTrack(trackIndex);
        },
        .addBlankMidiClipInRange = [this](int32_t trackIndex, double startSeconds, double endSeconds) {
            addBlankMidiClipInRange(trackIndex, startSeconds, endSeconds);
        },
        .addEmptyAudioClipInRange = [this](int32_t trackIndex, double startSeconds, double endSeconds) {
            addEmptyAudioClipInRange(trackIndex, startSeconds, endSeconds);
        },
        .removeClip = [this](int32_t trackIndex, int32_t clipId) {
            removeClipFromTrack(trackIndex, clipId);
        },
        .clearAllClips = [this](int32_t trackIndex) {
            clearAllClipsFromTrack(trackIndex);
        },
        .moveClipAbsolute = [this](int32_t trackIndex, int32_t clipId, double seconds) {
            moveClipAbsolute(trackIndex, clipId, seconds);
        },
        .showMidiClipDump = [this](int32_t trackIndex, int32_t clipId) {
            showMidiClipDump(trackIndex, clipId);
        },
        .showAudioClipEvents = [this](int32_t trackIndex, int32_t clipId) {
            showAudioClipEvents(trackIndex, clipId);
        },
        .showPianoRoll = [this](int32_t trackIndex, int32_t clipId) {
            showPianoRoll(trackIndex, clipId);
        },
        .showMasterTrackDump = [this]() {
            showMasterMetaDump();
        },
        .showClipsWindow = [this](int32_t trackIndex) {
            // Reuse the existing "Edit Clips..." table window (owned by sequenceEditor_) rather
            // than duplicating it -- that window stays seconds-based regardless of which unified
            // timeline is currently visible.
            sequenceEditor_.showWindow(trackIndex);
            refreshSequenceEditorForTrack(trackIndex);
        },
        .renderLegendContent = [this](int32_t trackIndex, const ImRect& legendArea) {
            renderTrackLegendContent(trackIndex, legendArea);
        },
        .secondsToBeats = [this](double seconds) {
            return tempoMap_.secondsToBeats(seconds);
        },
        .beatsToSeconds = [this](double beats) {
            return tempoMap_.beatsToSeconds(beats);
        },
        .tempoMap = &tempoMap_,
        .timelineUnitsLabel = "beats",
        .uiScale = uiScale,
        .legendWidth = legendWidth,
    };
}

void TimelineEditor::render(float uiScale) {
    currentUiScale_ = uiScale;
    if (pendingFullReset_) {
        pendingFullReset_ = false;
        sequenceEditor_.reset();
        beatsSequenceEditor_.reset();
        trackContentSignatures_.clear();
        masterTrackSignature_.clear();
        masterTrackSectionCreated_ = false;
    }
    syncExternalTimelineChanges();
    auto context = buildRenderContext(uiScale);
    auto beatsContext = buildBeatsRenderContext(uiScale, context.legendWidth);
    renderTrackList(context, beatsContext);
    sequenceEditor_.render(context);

    // Render InstanceDetails with context
    InstanceDetails::RenderContext detailsContext{
        .buildTrackInstance = [this](int32_t instanceId) -> std::optional<TrackInstance> {
            if (callbacks_.buildTrackInstanceInfo)
                return callbacks_.buildTrackInstanceInfo(instanceId);
            return std::nullopt;
        },
        .savePluginState = [this](int32_t instanceId) {
            if (callbacks_.savePluginState)
                callbacks_.savePluginState(instanceId);
        },
        .loadPluginState = [this](int32_t instanceId) {
            if (callbacks_.loadPluginState)
                callbacks_.loadPluginState(instanceId);
        },
        .removeInstance = [this](int32_t instanceId) {
            if (callbacks_.handleRemoveInstance)
                callbacks_.handleRemoveInstance(instanceId);
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .onWindowClosed = [this](int32_t instanceId) {
            if (callbacks_.onInstanceDetailsClosed)
                callbacks_.onInstanceDetailsClosed(instanceId);
        },
        .uiScale = uiScale,
    };
    instanceDetails_.render(detailsContext);

    // Render MidiDumpWindow with context
    MidiDumpWindow::RenderContext midiDumpContext{
        .reloadClip = [this](int32_t trackIndex, int32_t clipId) {
            return buildMidiClipDumpData(trackIndex, clipId);
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .applyEdits = [this](const MidiDumpWindow::EditPayload& payload, std::string& error) {
            return applyMidiClipEdits(payload, error);
        },
        .uiScale = uiScale,
    };
    midiDumpWindow_.render(midiDumpContext);

    AudioEventListEditor::RenderContext audioEventContext{
        .reloadClip = [this](int32_t trackIndex, int32_t clipId) {
            return buildAudioEventListData(trackIndex, clipId);
        },
        .buildExternalReferenceOptions = [this](int32_t trackIndex, int32_t clipId) {
            auto& appModel = uapmd::AppModel::instance();
            return ::uapmd::gui::buildExternalReferenceOptions(
                trackIndex,
                clipId,
                audioEventListEditor_,
                appModel.getTimelineTracks(),
                appModel.masterTrackMarkers()
            );
        },
        .validateMarkerReference = [this](int32_t trackIndex, int32_t clipId, const std::string& markerId,
            uapmd::AudioWarpReferenceType referenceType, const std::string& referenceClipId, const std::string& referenceMarkerId) {
            auto& appModel = uapmd::AppModel::instance();
            return ::uapmd::gui::validateMarkerReferenceSelection(
                trackIndex,
                clipId,
                markerId,
                referenceType,
                referenceClipId,
                referenceMarkerId,
                audioEventListEditor_,
                appModel.getTimelineTracks(),
                appModel.masterTrackMarkers()
            );
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .applyEdits = [this](const AudioEventListEditor::EditPayload& payload, std::string& error) {
            return applyAudioClipEdits(payload, error);
        },
        .uiScale = uiScale,
    };
    audioEventListEditor_.render(audioEventContext);

    PianoRollEditor::RenderContext pianoCtx;
    pianoCtx.uiScale = uiScale;
    pianoCtx.applyEdits = [this](int32_t trackIndex, int32_t clipId,
                                  std::vector<uapmd_ump_t> newEvents,
                                  std::vector<uint64_t>    newTicks,
                                  std::string&             error) -> bool {
        return applyPianoRollEdits(trackIndex, clipId,
                                   std::move(newEvents), std::move(newTicks), error);
    };
    pianoCtx.reloadPreview = [](int32_t trackIndex, int32_t clipId) -> std::shared_ptr<ClipPreview> {
        auto& appModel = uapmd::AppModel::instance();
        auto tracks = appModel.getTimelineTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) ||
                !tracks[trackIndex])
            return nullptr;
        auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
        if (!clip) return nullptr;
        return createMidiClipPreview(trackIndex, *clip, 0.0);
    };
    pianoCtx.previewNoteOn = [](int32_t trackIndex, int midiNote) {
        auto& seq = uapmd::AppModel::instance().sequencer();
        auto tracksRef = seq.engine()->tracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracksRef.size())) return;
        auto* track = tracksRef[trackIndex];
        if (!track) return;
        for (int32_t instanceId : track->orderedInstanceIds())
            if (seq.engine()->getPluginInstance(instanceId))
                seq.engine()->sendNoteOn(instanceId, midiNote);
    };
    pianoCtx.previewNoteOff = [](int32_t trackIndex, int midiNote) {
        auto& seq = uapmd::AppModel::instance().sequencer();
        auto tracksRef = seq.engine()->tracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracksRef.size())) return;
        auto* track = tracksRef[trackIndex];
        if (!track) return;
        for (int32_t instanceId : track->orderedInstanceIds())
            if (seq.engine()->getPluginInstance(instanceId))
                seq.engine()->sendNoteOff(instanceId, midiNote);
    };
    pianoCtx.getTrackPluginParameters = [this](int32_t trackIndex) {
        return getPluginParametersForTrack(trackIndex);
    };
    pianoRollEditor_.render(pianoCtx);
}

void TimelineEditor::renderPluginSelectorWindow(float uiScale) {
    if (!showPluginSelectorWindow_)
        return;

    const std::string windowId = "PluginSelector";
    if (setNextChildWindowSize_)
        setNextChildWindowSize_(windowId, ImVec2(720.0f, 600.0f));
    if (ImGui::Begin("Plugin Selector", &showPluginSelectorWindow_)) {
        if (updateChildWindowSizeState_)
            updateChildWindowSizeState_(windowId);

        pluginSelector_.setScanning(uapmd::AppModel::instance().isScanning());
        pluginSelector_.render();
    }
    ImGui::End();
}

void TimelineEditor::renderPluginGraphWindow(float uiScale) {
    pluginGraphEditor_.render(uiScale, setNextChildWindowSize_, updateChildWindowSizeState_);
}

void TimelineEditor::renderTrackList(const SequenceEditor::RenderContext& context, const BeatsSequenceEditor::RenderContext& beatsContext) {
    auto& appModel = uapmd::AppModel::instance();
    // Toolbar row: view switcher + navigation controls (zoom slider, position controller).
    // Rendered outside TrackListScroll so they stay visible regardless of track scrolling;
    // the button is vertically centered against the taller navigator row. The position
    // controller is anchored to the track content column (row left + legend width) so it
    // lines up with the clips below; the zoom slider gets only what remains after the button.
    const float navRowTopY = ImGui::GetCursorPosY();
    const float navBarStartX = ImGui::GetCursorScreenPos().x + context.legendWidth;
    ImGui::SetCursorPosY(navRowTopY + (kNavigatorHeightPt * context.uiScale - ImGui::GetFrameHeight()) * 0.5f);
    if (ImGui::Button(timelineViewMode_ == TimelineViewMode::AbsoluteTime ? "View: Seconds" : "View: Beats")) {
        timelineViewMode_ = (timelineViewMode_ == TimelineViewMode::AbsoluteTime)
            ? TimelineViewMode::BeatsTicks : TimelineViewMode::AbsoluteTime;
        beatsSequenceEditor_.invalidateTimeline();
        sequenceEditor_.invalidateTimeline();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Switch between absolute-time and beats/ticks track list views");
    ImGui::SameLine();
    ImGui::SetCursorPosY(navRowTopY);
    if (timelineViewMode_ == TimelineViewMode::AbsoluteTime)
        sequenceEditor_.renderNavigator(context, navBarStartX);
    else
        beatsSequenceEditor_.renderNavigator(beatsContext, navBarStartX);
    ImGui::Spacing();

    const float footerHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("TrackListScroll", ImVec2(0, -footerHeight), true, ImGuiWindowFlags_None);

    // Update master track clips if snapshot changed (runs every frame, cheap)
    renderMasterTrackRow(context);

    if (timelineViewMode_ == TimelineViewMode::AbsoluteTime) {
        const float totalHeight = sequenceEditor_.getUnifiedTimelineHeight(context.uiScale);
        sequenceEditor_.renderUnifiedTimeline(context, totalHeight);
    } else {
        const float totalHeight = beatsSequenceEditor_.getUnifiedTimelineHeight(beatsContext.uiScale);
        beatsSequenceEditor_.renderUnifiedTimeline(beatsContext, totalHeight);
    }

    ImGui::EndChild();

    if (ImGui::Button(icons::Plus)) {
        int32_t newIndex = appModel.addTrack();
        if (newIndex >= 0)
            refreshSequenceEditorForTrack(newIndex);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add track");
    if (callbacks_.showMixerMonitor) {
        ImGui::SameLine();
        if (ImGui::Button("Mixer Monitor"))
            callbacks_.showMixerMonitor();
    }
    if (callbacks_.showPluginInstances) {
        ImGui::SameLine();
        if (ImGui::Button("Plugin Instances"))
            callbacks_.showPluginInstances();
    }
}

void TimelineEditor::renderMasterTrackRow(const SequenceEditor::RenderContext& context) {
    auto& appModel = uapmd::AppModel::instance();

    // Always keep the merged snapshot current for tempo-segment conversion.
    auto snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
        appModel.buildMasterTrackSnapshot());
    masterTrackSnapshot_ = snapshot;

    auto* masterTrack = appModel.getMasterTimelineTrack();
    if (!masterTrack) return;

    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

    // Gather individual clips; sort by timeline position so lanes are stable.
    auto clips = masterTrack->clipManager().getAllClips();
    std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
        return a.position.samples < b.position.samples;
    });

    // Signature covers each clip's id, position, duration, and name.
    std::string signature;
    for (const auto& clip : clips)
        signature += std::format("{}:{}:{}:{};",
            clip.clipId, clip.position.samples, clip.durationSamples, clip.name);

    const bool signatureChanged = (signature != masterTrackSignature_);
    if (!signatureChanged && masterTrackSectionCreated_) return;

    if (signatureChanged) {
        masterTrackSignature_ = signature;
        rebuildTempoSegments(masterTrackSnapshot_);
    }

    // Build one ClipRow per master-track clip so they can be moved / deleted independently.
    // Master-track clips are always treated as an audio-type span for the timeline widget (they
    // have no MIDI note content of their own -- just tempo/time-signature meta events), so the
    // beats-domain row below uses the audio-clip endpoint formula (tempo-varying width), matching
    // the seconds row's uniform treatment here.
    std::vector<SequenceEditor::ClipRow> rows;
    std::vector<BeatsSequenceEditor::ClipRow> beatsRows;
    for (const auto& clip : clips) {
        SequenceEditor::ClipRow row;
        row.clipId              = clip.clipId;
        row.trackReferenceId   = "master_track";
        row.anchorReferenceId.clear();
        row.anchorOrigin       = "Start";
        row.position           = "+0.000s";
        row.isMidiClip         = false;
        row.isMasterTrack      = true;
        row.name     = clip.name.empty() ? "SMF Meta Events" : clip.name;
        row.filename = clip.filepath.empty() ? "-" : std::filesystem::path(clip.filepath).filename().string();
        row.filepath = clip.filepath;

        const double startSeconds    = static_cast<double>(clip.position.samples) / sampleRate;
        const double durationSeconds = clip.durationSamples > 0
            ? static_cast<double>(clip.durationSamples) / sampleRate
            : std::max(1.0, snapshot->maxTimeSeconds - startSeconds);
        row.duration       = std::format("{:.3f}s", durationSeconds);
        row.timelineStart  = toTimelineFrame(startSeconds);
        row.timelineEnd    = std::max(row.timelineStart + 1,
                                      toTimelineFrame(startSeconds + durationSeconds));

        // Extract per-clip tempo / time-signature events for the waveform preview.
        auto sourceNode = masterTrack->getSourceNode(clip.sourceNodeInstanceId);
        auto* midiNode  = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
        std::vector<ClipPreview::TempoPoint> tempoPoints;
        std::vector<ClipPreview::TimeSignaturePoint> sigPoints;
        if (midiNode) {
            const auto& tSamples = midiNode->tempoChangeSamples();
            const auto& tEvents  = midiNode->tempoChanges();
            for (size_t i = 0; i < std::min(tSamples.size(), tEvents.size()); ++i)
                tempoPoints.push_back({static_cast<double>(tSamples[i]) / sampleRate, tEvents[i].bpm});

            const auto& sSamples = midiNode->timeSignatureChangeSamples();
            const auto& sEvents  = midiNode->timeSignatureChanges();
            for (size_t i = 0; i < std::min(sSamples.size(), sEvents.size()); ++i)
                sigPoints.push_back({static_cast<double>(sSamples[i]) / sampleRate,
                                     sEvents[i].numerator, sEvents[i].denominator});
        }
        row.customPreview = createMasterMetaPreview(std::move(tempoPoints), std::move(sigPoints), durationSeconds);

        BeatsSequenceEditor::ClipRow beatsRow;
        beatsRow.clipId         = clip.clipId;
        beatsRow.isMidiClip     = false;
        beatsRow.isMasterTrack  = true;
        beatsRow.name           = row.name;
        beatsRow.filepath       = clip.filepath;
        const double startBeats = tempoMap_.secondsToBeats(startSeconds);
        const double endBeats   = tempoMap_.secondsToBeats(startSeconds + durationSeconds);
        beatsRow.timelineStartTicks = toBeatFrame(startBeats);
        beatsRow.timelineEndTicks   = std::max(beatsRow.timelineStartTicks + 1, toBeatFrame(endBeats));
        beatsRow.customPreview  = row.customPreview;
        beatsRows.push_back(std::move(beatsRow));

        rows.push_back(std::move(row));
    }

    if (rows.empty()) {
        // Placeholder row when the master track has no clips yet.
        SequenceEditor::ClipRow row;
        row.clipId         = kMasterTrackClipId;
        row.trackReferenceId = "master_track";
        row.isMasterTrack  = true;
        row.name           = "No Meta Events";
        row.filename       = "-";
        const double dur   = std::max(1.0, snapshot->maxTimeSeconds);
        row.timelineStart  = toTimelineFrame(0.0);
        row.timelineEnd    = std::max(row.timelineStart + 1, toTimelineFrame(dur));
        row.customPreview  = createMasterMetaPreview({}, {}, dur);

        BeatsSequenceEditor::ClipRow beatsRow;
        beatsRow.clipId        = kMasterTrackClipId;
        beatsRow.isMasterTrack = true;
        beatsRow.name          = row.name;
        const double startBeats = tempoMap_.secondsToBeats(0.0);
        const double endBeats   = tempoMap_.secondsToBeats(dur);
        beatsRow.timelineStartTicks = toBeatFrame(startBeats);
        beatsRow.timelineEndTicks   = std::max(beatsRow.timelineStartTicks + 1, toBeatFrame(endBeats));
        beatsRow.customPreview = row.customPreview;
        beatsRows.push_back(std::move(beatsRow));

        rows.push_back(std::move(row));
    }

    sequenceEditor_.refreshClips(uapmd::kMasterTrackIndex, rows);
    beatsSequenceEditor_.refreshClips(uapmd::kMasterTrackIndex, beatsRows);
    masterTrackSectionCreated_ = true;

    // Only refresh regular tracks when tempo segments actually changed.
    if (signatureChanged) {
        auto tracks = appModel.getTimelineTracks();
        for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
            if (!appModel.isTrackHidden(i))
                refreshSequenceEditorForTrack(i);
        }
    }
}

void TimelineEditor::renderTrackLegendContent(int32_t trackIndex, const ImRect& legendArea) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto tracksRef = sequencer.engine()->tracks();
    SequencerTrack* track = nullptr;
    if (trackIndex == uapmd::kMasterTrackIndex)
        track = sequencer.engine()->masterTrack();
    else if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracksRef.size()))
        track = tracksRef[trackIndex];

    std::vector<int32_t> validInstances;
    if (track) {
        auto& ids = track->orderedInstanceIds();
        validInstances.reserve(ids.size());
        for (int32_t instanceId : ids)
            if (sequencer.engine()->getPluginInstance(instanceId))
                validInstances.push_back(instanceId);
    }

    std::string pluginLabel = trackIndex == uapmd::kMasterTrackIndex
        ? "Add Master Plugin"
        : "Add Plugin";
    if (!validInstances.empty())
        if (auto* instance = sequencer.engine()->getPluginInstance(validInstances.front()))
            pluginLabel = instance->displayName();

    std::string popupId = std::format("TrackActions##{}", trackIndex);
    std::string clipPopupId = std::format("ClipActions##{}", trackIndex);

    const float pad = 4.0f * currentUiScale_;
    const float legendWidth = legendArea.GetWidth();
    ImGui::PushClipRect(legendArea.Min, legendArea.Max, true);
    ImGui::SetCursorScreenPos(ImVec2(legendArea.Min.x + pad, legendArea.Min.y + pad));
    ImGui::PushID(trackIndex);

    // Row 1: Clips + Graph + Gain Slider + [Bypass]
    if (renderIconButtonWithTooltip(std::format("{}##LegClips{}", icons::Clips, trackIndex).c_str(), "Edit clips"))
        ImGui::OpenPopup(clipPopupId.c_str());
    ImGui::SameLine();
    if (renderIconButtonWithTooltip(std::format("{}##LegGraph{}", icons::Graph, trackIndex).c_str(), "Show track graph")) {
        if (pluginGraphEditor_.isVisible(trackIndex))
            pluginGraphEditor_.hideTrack(trackIndex);
        else
            pluginGraphEditor_.showTrack(trackIndex);
    }
    if (track) {
        ImGui::SameLine();
        const float iconButtonWidth = std::max({
            ImGui::CalcTextSize(icons::Clips).x,
            ImGui::CalcTextSize(icons::Graph).x,
            ImGui::CalcTextSize(icons::ToggleOn).x,
            ImGui::CalcTextSize(icons::DeleteTrack).x,
        }) + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float sliderWidth = iconButtonWidth * 1.5f;
        const float gainDb = linearGainToSliderDb(track->trackGain());
        float sliderPos = dbToSliderPos(gainDb);
        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderFloat(
                std::format("##LegGain{}", trackIndex).c_str(),
                &sliderPos,
                0.0f, 1.0f,
                sliderPos <= 0.0f ? "Mute" : "",
                ImGuiSliderFlags_NoInput))
            track->trackGain(sliderDbToLinearGain(sliderPosToDb(sliderPos)));
        if (ImGui::IsItemHovered()) {
            const double linearGain = track->trackGain();
            if (linearGain <= 0.0)
                ImGui::SetTooltip("Track volume: muted");
            else
                ImGui::SetTooltip("Track volume: %.1f dB (%.3fx)", linearGainToSliderDb(linearGain), linearGain);
        }

        ImGui::SameLine();
        bool bypassed = track->bypassed();
        if (bypassed)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const char* toggleIcon = bypassed ? uapmd::gui::icons::ToggleOff : uapmd::gui::icons::ToggleOn;
        if (contextActionButton(std::format("{}##LegByp{}", toggleIcon, trackIndex).c_str(), ImVec2(0.0f, 0.0f),
                bypassed ? "Track bypassed (click to enable)" : "Bypass track"))
            track->bypassed(!bypassed);
        if (bypassed)
            ImGui::PopStyleColor();
    }

    // Row 2: Freeze policy + Plugin context button + Delete on the right
    ImGui::SetCursorScreenPos(ImVec2(legendArea.Min.x + pad, ImGui::GetCursorScreenPos().y));
    const float buttonWidth = legendWidth - pad * 2;
    float deleteButtonWidth = 0.0f;
    if (track && trackIndex != uapmd::kMasterTrackIndex)
        deleteButtonWidth = ImGui::CalcTextSize(icons::DeleteTrack).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float freezePolicyButtonWidth = ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float freezePolicyWidth = track && trackIndex != uapmd::kMasterTrackIndex
        ? freezePolicyButtonWidth
        : 0.0f;
    const float pluginButtonWidth = std::max(
        0.0f,
        buttonWidth - freezePolicyWidth - deleteButtonWidth -
            (freezePolicyWidth > 0.0f ? itemSpacing : 0.0f) -
            (deleteButtonWidth > 0.0f ? itemSpacing : 0.0f));

    if (track && trackIndex != uapmd::kMasterTrackIndex) {
        auto& appModel = uapmd::AppModel::instance();
        auto& frozenTrackManager = appModel.sequencer().engine()->frozenTrackManager();
        const auto currentPolicy = frozenTrackManager.freezePolicyForTrack(trackIndex);
        const char* policyLabel = "Off";
        const char* policyTooltip = "Track freezing: Off (click for Auto)";
        uapmd::FrozenTrackManager::FreezePolicy nextPolicy = uapmd::FrozenTrackManager::FreezePolicy::Auto;
        switch (currentPolicy) {
            case uapmd::FrozenTrackManager::FreezePolicy::Auto:
                policyLabel = "Auto";
                policyTooltip = "Track freezing: Auto (click for On)";
                nextPolicy = uapmd::FrozenTrackManager::FreezePolicy::On;
                break;
            case uapmd::FrozenTrackManager::FreezePolicy::On:
                policyLabel = "On";
                policyTooltip = "Track freezing: On (click for Off)";
                nextPolicy = uapmd::FrozenTrackManager::FreezePolicy::Off;
                break;
            case uapmd::FrozenTrackManager::FreezePolicy::Off:
            default:
                break;
        };
        if (contextActionButton(
                std::format("{}##LegFreeze{}", policyLabel, trackIndex).c_str(),
            ImVec2(freezePolicyButtonWidth, 0.0f),
                policyTooltip) &&
            frozenTrackManager.setFreezePolicyForTrack(trackIndex, nextPolicy))
            appModel.markProjectDirty();
        ImGui::SameLine();
    }
    if (contextActionButton(std::format("{} {}##LegPlug{}", icons::ContextMenu, pluginLabel, trackIndex).c_str(), ImVec2(pluginButtonWidth, 0)))
        ImGui::OpenPopup(popupId.c_str());
    if (deleteButtonWidth > 0.0f) {
        ImGui::SameLine();
        if (contextActionButton(std::format("{}##LegDel{}", uapmd::gui::icons::DeleteTrack, trackIndex).c_str(), ImVec2(deleteButtonWidth, 0.0f),
                "Delete track"))
            deleteTrack(trackIndex);
    }

    // Clips popup
    if (ImGui::BeginPopup(clipPopupId.c_str())) {
        const bool isMasterTrack = (trackIndex == uapmd::kMasterTrackIndex);
        if (contextActionMenuItem("Edit Clips...", sequenceEditor_.isVisible(trackIndex))) {
            sequenceEditor_.showWindow(trackIndex);
            refreshSequenceEditorForTrack(trackIndex);
        }
        ImGui::Separator();
        if (contextActionMenuItem("Add an Empty MIDI2 Clip"))
            addBlankMidi2ClipToTrack(trackIndex);
        if (!isMasterTrack) {
            ImGui::Separator();
            if (contextActionMenuItem("Add Audio Clip from File..."))
                addAudioClipToTrack(trackIndex);
        }
        ImGui::Separator();
        if (contextActionMenuItem("Add a MIDI Clip from File..."))
            addSmfClipToTrack(trackIndex);
        if (!isMasterTrack) {
            if (contextActionMenuItem("Add MIDI2 Clip from File..."))
                addSmf2ClipToTrack(trackIndex);
            ImGui::Separator();
            if (contextActionMenuItem("Clear All"))
                clearAllClipsFromTrack(trackIndex);
        }
        ImGui::EndPopup();
    }

    // Plugin popup
    if (ImGui::BeginPopup(popupId.c_str())) {
        if (track) {
            for (int i = 0; i < static_cast<int>(validInstances.size()); ++i) {
                int32_t instanceId = validInstances[static_cast<size_t>(i)];
                auto* instance = sequencer.engine()->getPluginInstance(instanceId);
                if (!instance)
                    continue;
                std::string pluginName = instance->displayName();

                bool detailsVisible = instanceDetails_.isVisible(instanceId);
                if (contextActionMenuItem(std::format("{} {} Details##details{}", detailsVisible ? "Hide" : "Show", pluginName, instanceId).c_str())) {
                    if (detailsVisible)
                        instanceDetails_.hideWindow(instanceId);
                    else
                        instanceDetails_.showWindow(instanceId);
                    ImGui::CloseCurrentPopup();
                }

                if (callbacks_.buildTrackInstanceInfo) {
                    if (auto trackInstance = callbacks_.buildTrackInstanceInfo(instanceId)) {
                        bool disableShowUIButton = !trackInstance->hasUI;
                        if (disableShowUIButton)
                            ImGui::BeginDisabled();
                        if (contextActionMenuItem(std::format("{} {} GUI##gui{}", trackInstance->uiVisible ? "Hide" : "Show", pluginName, instanceId).c_str())) {
                            if (trackInstance->uiVisible)
                                uapmd::AppModel::instance().hidePluginUI(instanceId);
                            else
                                uapmd::AppModel::instance().requestShowPluginUI(instanceId);
                            ImGui::CloseCurrentPopup();
                        }
                        if (disableShowUIButton)
                            ImGui::EndDisabled();
                    }
                }
            }

            if (!validInstances.empty()) {
                ImGui::Separator();
                for (int i = 0; i < static_cast<int>(validInstances.size()); ++i) {
                    int32_t instanceId = validInstances[static_cast<size_t>(i)];
                    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
                    if (!instance)
                        continue;
                    if (contextActionMenuItem(std::format("Delete {} (at [{}])##delete{}", instance->displayName(), i + 1, instanceId).c_str())) {
                        if (callbacks_.handleRemoveInstance)
                            callbacks_.handleRemoveInstance(instanceId);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::Separator();
            }
        }

        const char* addPluginLabel = trackIndex == uapmd::kMasterTrackIndex
            ? "Add Master Plugin"
            : "Add Plugin";
        if (contextActionMenuItem(addPluginLabel)) {
            if (trackIndex == uapmd::kMasterTrackIndex)
                pluginSelector_.setTargetMasterTrack(trackIndex);
            else
                pluginSelector_.setTargetTrackIndex(trackIndex);
            showPluginSelectorWindow_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    ImGui::PopClipRect();
}

void TimelineEditor::renderTrackRow(int32_t /*trackIndex*/, const SequenceEditor::RenderContext& /*context*/) {
    // Tracks are rendered via renderUnifiedTimeline; this stub satisfies the vtable.
}

void TimelineEditor::deleteTrack(int32_t trackIndex) {
    uapmd::AppModel::instance().removeTrack(trackIndex);
}

void TimelineEditor::refreshAllSequenceEditorTracks() {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
        if (appModel.isTrackHidden(i))
            continue;
        refreshSequenceEditorForTrack(i);
    }
}

void TimelineEditor::syncExternalTimelineChanges() {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    for (auto it = trackContentSignatures_.begin(); it != trackContentSignatures_.end();) {
        if (it->first < 0 || it->first >= static_cast<int32_t>(tracks.size()))
            it = trackContentSignatures_.erase(it);
        else
            ++it;
    }

    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
        if (appModel.isTrackHidden(i))
            continue;
        const auto signature = buildTrackContentSignature(i);
        auto it = trackContentSignatures_.find(i);
        if (it != trackContentSignatures_.end() && it->second == signature)
            continue;
        refreshSequenceEditorForTrack(i);
    }
}

void TimelineEditor::handleTrackLayoutChange(const uapmd::AppModel::TrackLayoutChange& change) {
    switch (change.type) {
        case uapmd::AppModel::TrackLayoutChange::Type::Added:
            refreshSequenceEditorForTrack(change.trackIndex);
            break;
        case uapmd::AppModel::TrackLayoutChange::Type::Removed:
            // Defer the reset to the start of the next render() call. Resetting here
            // would destroy unified_.timeline while DrawTimeline() may still be on the
            // call stack (deletion can be triggered from within the legend callback).
            pendingFullReset_ = true;
            break;
        case uapmd::AppModel::TrackLayoutChange::Type::Cleared:
            sequenceEditor_.reset();
            beatsSequenceEditor_.reset();
            trackContentSignatures_.clear();
            masterTrackSignature_.clear();
            masterTrackSectionCreated_ = false;
            break;
    }
}

void TimelineEditor::rebuildTempoSegments(const std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot>& snapshot) {
    if (!snapshot || snapshot->tempoPoints.empty()) {
        tempoMap_.clear();
        timelineUnitsLabel_ = "seconds";
        return;
    }

    std::vector<uapmd::TempoMap::TempoPoint> tempoPoints;
    tempoPoints.reserve(snapshot->tempoPoints.size());
    for (const auto& p : snapshot->tempoPoints)
        tempoPoints.push_back({p.timeSeconds, p.bpm});

    std::vector<uapmd::TempoMap::TimeSignaturePoint> timeSignaturePoints;
    timeSignaturePoints.reserve(snapshot->timeSignaturePoints.size());
    for (const auto& p : snapshot->timeSignaturePoints)
        timeSignaturePoints.push_back({p.timeSeconds, p.signature});

    tempoMap_.rebuild(tempoPoints, timeSignaturePoints, kDisplayDefaultBpm);
    timelineUnitsLabel_ = tempoMap_.hasTempoData() ? "beats" : "seconds";

    Logger::global()->logDiagnostic("[TEMPO MAP] Rebuilt from %d tempo point(s), %d time-signature point(s)",
        tempoPoints.size(), timeSignaturePoints.size());
}

void TimelineEditor::fitTimelineToContent(float uiScale) {
    auto bounds = uapmd::AppModel::instance().timelineContentBounds();
    if (!bounds.hasContent || bounds.durationSeconds <= 0.0)
        return;

    const float secondsWidth = sequenceEditor_.lastVisibleWidth();
    sequenceEditor_.fitToContent(bounds.durationSeconds, secondsWidth, uiScale);

    // tempoMap_ must already be built (e.g. via refreshAllSequenceEditorTracks, called just
    // before this) -- it's cleared by invalidateMasterTrackSnapshot(), so this must run
    // before that call, not after.
    const double durationBeats = tempoMap_.secondsToBeats(bounds.durationSeconds);
    if (durationBeats > 0.0) {
        const float beatsWidth = beatsSequenceEditor_.lastVisibleWidth();
        beatsSequenceEditor_.fitToContent(durationBeats, beatsWidth, uiScale);
    }
}

void TimelineEditor::invalidateMasterTrackSnapshot() {
    masterTrackSnapshot_.reset();
    masterTrackSignature_.clear();
    tempoMap_.clear();
    timelineUnitsLabel_ = "seconds";
}

void TimelineEditor::refreshSequenceEditorForTrack(int32_t trackIndex) {
    if (trackIndex < 0)
        return;
    auto& appModel = uapmd::AppModel::instance();

    // Ensure the tempo map is built before computing clip positions
    if (tempoMap_.empty()) {
        auto snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
            appModel.buildMasterTrackSnapshot());
        rebuildTempoSegments(snapshot);
    }
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    auto* track = tracks[trackIndex];
    auto clips = track->clipManager().getAllClips();

    // Sort clips by clipId to ensure chronological order
    std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
        return a.clipId > b.clipId;
    });

    std::vector<SequenceEditor::ClipRow> displayClips;
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

    for (const auto& clip : clips) {
        SequenceEditor::ClipRow row;
        row.clipId = clip.clipId;
        row.referenceId = clip.referenceId;
        row.trackReferenceId = track->referenceId();
        const auto timeReference = clip.timeReference(appModel.sampleRate());
        row.anchorReferenceId = timeReference.referenceId;
        row.anchorOrigin = (timeReference.type == uapmd::TimeReferenceType::ContainerEnd) ? "End" : "Start";

        double positionSeconds = timeReference.offset;
        row.position = std::format("{:+.3f}s", positionSeconds);

        double durationSeconds = static_cast<double>(clip.durationSamples) / appModel.sampleRate();
        row.duration = std::format("{:.3f}s", durationSeconds);

        row.name = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
        row.filepath = clip.filepath;
        if (clip.filepath.empty()) {
            row.filename = clip.clipType == uapmd::ClipType::Midi ? "(MIDI UMP)" : "(no audio file)";
        } else {
            size_t lastSlash = clip.filepath.find_last_of("/\\");
            row.filename = (lastSlash != std::string::npos)
                ? clip.filepath.substr(lastSlash + 1)
                : clip.filepath;
        }

        row.isMidiClip = (clip.clipType == uapmd::ClipType::Midi);
        if (row.isMidiClip)
            row.mimeType = "audio/midi";
        else
            row.mimeType = "";

        auto absolutePosition = clip.position;
        double absoluteStartSeconds = static_cast<double>(absolutePosition.samples) / sampleRate;
        double durationSecondsExact = static_cast<double>(clip.durationSamples) / sampleRate;
        row.timelineStart = toTimelineFrame(absoluteStartSeconds);
        int32_t endFrame = toTimelineFrame(absoluteStartSeconds + durationSecondsExact);
        if (endFrame <= row.timelineStart)
            endFrame = row.timelineStart + 1;
        row.timelineEnd = endFrame;

        displayClips.push_back(row);
    }

    sequenceEditor_.refreshClips(trackIndex, displayClips);
    trackContentSignatures_[trackIndex] = buildTrackContentSignature(trackIndex);

    // Keep the beats editor in sync with every call site that refreshes the seconds editor,
    // rather than requiring each clip-mutation call site to remember to refresh both -- that
    // split responsibility is exactly what let the beats editor silently go stale earlier.
    refreshBeatsSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::refreshBeatsSequenceEditorForTrack(int32_t trackIndex) {
    if (trackIndex < 0)
        return;
    auto& appModel = uapmd::AppModel::instance();

    // Ensure the tempo map is built before computing clip positions
    if (tempoMap_.empty()) {
        auto snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
            appModel.buildMasterTrackSnapshot());
        rebuildTempoSegments(snapshot);
    }
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    auto* track = tracks[trackIndex];
    auto clips = track->clipManager().getAllClips();

    // Sort clips by clipId to ensure chronological order (matches refreshSequenceEditorForTrack)
    std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
        return a.clipId > b.clipId;
    });

    std::vector<BeatsSequenceEditor::ClipRow> displayClips;
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

    for (const auto& clip : clips) {
        BeatsSequenceEditor::ClipRow row;
        row.clipId = clip.clipId;
        row.name = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
        row.filepath = clip.filepath;
        row.isMidiClip = (clip.clipType == uapmd::ClipType::Midi);

        // Position: a clip's anchor is always a real-world-seconds concept, so it always needs
        // the tempo-map integration regardless of clip type.
        const double absoluteStartSeconds = static_cast<double>(clip.position.samples) / sampleRate;
        const double startBeats = tempoMap_.secondsToBeats(absoluteStartSeconds);
        row.timelineStartTicks = toBeatFrame(startBeats);

        if (row.isMidiClip) {
            // MIDI width is tempo-immune: derive it from the clip's own authored tempo
            // (durationSamples was captured at import time under clipTempo), bypassing the
            // live tempo map entirely. This is what makes MIDI clip width stay constant across
            // tempo changes, unlike audio (below).
            const double beatsLength = (static_cast<double>(clip.durationSamples) / sampleRate) * (clip.clipTempo / 60.0);
            row.timelineEndTicks = row.timelineStartTicks + toBeatFrame(beatsLength);
        } else {
            // Audio width varies with tempo: push the *endpoint* (not the duration) through the
            // tempo map, since durationSamples already bakes in any audio-warp resolution.
            const double durationSeconds = static_cast<double>(clip.durationSamples) / sampleRate;
            const double endBeats = tempoMap_.secondsToBeats(absoluteStartSeconds + durationSeconds);
            row.timelineEndTicks = toBeatFrame(endBeats);
        }
        row.timelineEndTicks = std::max(row.timelineEndTicks, row.timelineStartTicks + 1);

        displayClips.push_back(row);
    }

    beatsSequenceEditor_.refreshClips(trackIndex, displayClips);
}

std::string TimelineEditor::buildTrackContentSignature(int32_t trackIndex) const {
    auto tracks = uapmd::AppModel::instance().getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex])
        return {};

    auto* track = tracks[trackIndex];
    auto clips = track->clipManager().getAllClips();
    std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
        return a.clipId < b.clipId;
    });

    std::string signature = std::to_string(clips.size());
    signature.reserve(signature.size() + clips.size() * 96);
    for (const auto& clip : clips) {
        uint64_t midiHash = 0;
        auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
        if (auto* midiSource = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get()))
            midiHash = midiSourceFingerprint(*midiSource);

        signature += std::format("|{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            clip.clipId,
            static_cast<int>(clip.clipType),
            clip.position.samples,
            clip.durationSamples,
            clip.sourceNodeInstanceId,
            std::hash<std::string>{}(clip.referenceId),
            std::hash<std::string>{}(clip.timeReference(uapmd::AppModel::instance().sampleRate()).referenceId),
            static_cast<int>(clip.timeReference(uapmd::AppModel::instance().sampleRate()).type),
            std::bit_cast<uint64_t>(clip.timeReference(uapmd::AppModel::instance().sampleRate()).offset),
            clip.tickResolution,
            clip.name,
            midiHash);
    }
    return signature;
}

void TimelineEditor::resolveAllClipAnchors() {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    struct ClipRecord {
        uapmd::ClipManager* clipManager{nullptr};
        uapmd::ClipData clip;
    };

    std::unordered_map<ClipKey, ClipRecord, ClipKeyHash> clipRecords;
    clipRecords.reserve(128);

    auto collectTrack = [&clipRecords](uapmd::TimelineTrack* track) {
        if (!track)
            return;
        auto clips = track->clipManager().getAllClips();
        for (const auto& clip : clips)
            clipRecords.emplace(ClipKey{clip.referenceId}, ClipRecord{&track->clipManager(), clip});
    };

    for (int32_t trackIndex = 0; trackIndex < static_cast<int32_t>(tracks.size()); ++trackIndex)
        collectTrack(tracks[trackIndex]);
    collectTrack(appModel.getMasterTimelineTrack());

    std::unordered_map<ClipKey, uapmd::TimelinePosition, ClipKeyHash> resolvedPositions;
    std::unordered_set<ClipKey, ClipKeyHash> resolving;

    std::function<uapmd::TimelinePosition(const ClipKey&)> resolveClipPosition =
        [&](const ClipKey& key) -> uapmd::TimelinePosition {
            auto resolvedIt = resolvedPositions.find(key);
            if (resolvedIt != resolvedPositions.end())
                return resolvedIt->second;

            auto recordIt = clipRecords.find(key);
            if (recordIt == clipRecords.end())
                return {};

            const auto& clip = recordIt->second.clip;
            const auto timeReference = clip.timeReference(appModel.sampleRate());
            if (timeReference.referenceId.empty()) {
                auto resolved = uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
                resolvedPositions[key] = resolved;
                return resolved;
            }

            if (!resolving.insert(key).second) {
                auto resolved = uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
                resolvedPositions[key] = resolved;
                return resolved;
            }

            const ClipKey anchorKey{timeReference.referenceId};
            auto anchorIt = clipRecords.find(anchorKey);
            if (anchorIt == clipRecords.end()) {
                resolving.erase(key);
                auto resolved = uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
                resolvedPositions[key] = resolved;
                return resolved;
            }

            auto anchorPosition = resolveClipPosition(anchorKey);
            if (timeReference.type == uapmd::TimeReferenceType::ContainerEnd)
                anchorPosition.samples += anchorIt->second.clip.durationSamples;

            auto resolved = anchorPosition + uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
            resolvedPositions[key] = resolved;
            resolving.erase(key);
            return resolved;
        };

    for (const auto& [key, record] : clipRecords)
        record.clipManager->setClipPosition(record.clip.clipId, resolveClipPosition(key));
}

void TimelineEditor::addClipToTrack(int32_t trackIndex, const std::string& filepath) {
    auto handleFile = [this, trackIndex](const std::string& selectedFile) {
        std::filesystem::path path(selectedFile);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".midi2") {
            uapmd::TimelinePosition position;
            position.samples = 0;
            position.legacy_beats = 0.0;

            auto& appModel = uapmd::AppModel::instance();
            auto result = appModel.addClipToTrack(trackIndex, position, nullptr, selectedFile);

            if (!result.success) {
                platformError("Add MIDI Clip Failed",
                              "Could not add MIDI clip to track: " + result.error);
                return;
            }

            refreshSequenceEditorForTrack(trackIndex);
            return;
        }

        auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
        if (!reader) {
            platformError("Load Failed",
                          "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG");
            return;
        }

        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;

        auto& appModel = uapmd::AppModel::instance();
        auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);

        if (!result.success) {
            platformError("Add Clip Failed",
                          "Could not add clip to track: " + result.error);
            return;
        }

        refreshSequenceEditorForTrack(trackIndex);
    };

    if (!filepath.empty()) {
        handleFile(filepath);
        return;
    }

    std::vector<uapmd::DocumentFilter> filters{
        {"All Supported", {}, {"*.wav", "*.flac", "*.ogg", "*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"Audio Files",   {}, {"*.wav", "*.flac", "*.ogg"}},
        {"MIDI Files",    {}, {"*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"WAV Files",     {}, {"*.wav"}},
        {"FLAC Files",    {}, {"*.flac"}},
        {"OGG Files",     {}, {"*.ogg"}},
        {"All Files",     {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [handleFile = std::move(handleFile)](uapmd::DocumentPickResult result) mutable {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [handleFile = std::move(handleFile)](const std::filesystem::path& resolved) mutable {
                        handleFile(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Open Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::addClipToTrackAtPosition(int32_t trackIndex, const std::string& filepath, double positionSeconds) {
    auto handleFile = [this, trackIndex, positionSeconds](const std::string& selectedFile) {
        std::filesystem::path path(selectedFile);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto& appModel = uapmd::AppModel::instance();
        const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

        uapmd::TimelinePosition position;
        double clampedPositionSeconds = std::max(0.0, positionSeconds);
        position.samples = static_cast<int64_t>(std::llround(clampedPositionSeconds * sampleRate));
        position.legacy_beats = 0.0;

        if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".midi2") {
            auto result = appModel.addClipToTrack(trackIndex, position, nullptr, selectedFile);
            if (!result.success) {
                platformError("Add MIDI Clip Failed",
                              "Could not add MIDI clip to track: " + result.error);
                return;
            }
            refreshSequenceEditorForTrack(trackIndex);
            return;
        }

        auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
        if (!reader) {
            platformError("Load Failed",
                          "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG");
            return;
        }

        auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);
        if (!result.success) {
            platformError("Add Clip Failed",
                          "Could not add clip to track: " + result.error);
            return;
        }

        refreshSequenceEditorForTrack(trackIndex);
    };

    if (!filepath.empty()) {
        handleFile(filepath);
        return;
    }

    std::vector<uapmd::DocumentFilter> filters{
        {"All Supported", {}, {"*.wav", "*.flac", "*.ogg", "*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"Audio Files",   {}, {"*.wav", "*.flac", "*.ogg"}},
        {"MIDI Files",    {}, {"*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"WAV Files",     {}, {"*.wav"}},
        {"FLAC Files",    {}, {"*.flac"}},
        {"OGG Files",     {}, {"*.ogg"}},
        {"All Files",     {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [handleFile = std::move(handleFile)](uapmd::DocumentPickResult result) mutable {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [handleFile = std::move(handleFile)](const std::filesystem::path& resolved) mutable {
                        handleFile(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Open Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    if (trackIndex == uapmd::kMasterTrackIndex && clipId == kMasterTrackClipId) {
        return;
    }
    appModel.removeClipFromTrack(trackIndex, clipId);
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::clearAllClipsFromTrack(int32_t trackIndex) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    tracks[trackIndex]->clipManager().clearAll();
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::updateClip(int32_t trackIndex, int32_t clipId, const std::string& anchorReferenceId, const std::string& origin, const std::string& position) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    double offsetSeconds = 0.0;
    try {
        std::string posStr = position;
        if (!posStr.empty() && posStr.back() == 's')
            posStr = posStr.substr(0, posStr.length() - 1);
        offsetSeconds = std::stod(posStr);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse position string: " << position << std::endl;
        return;
    }

    uapmd::TimeReference anchor = origin == "End"
        ? uapmd::TimeReference::fromContainerEnd(anchorReferenceId, offsetSeconds)
        : uapmd::TimeReference::fromContainerStart(anchorReferenceId, offsetSeconds);
    auto* targetTrack = trackIndex == uapmd::kMasterTrackIndex
        ? appModel.getMasterTimelineTrack()
        : ((trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size())) ? tracks[trackIndex] : nullptr);
    if (!targetTrack)
        return;

    if (wouldCreateClipAnchorCycle(tracks, appModel.getMasterTimelineTrack(), trackIndex, clipId, anchor.referenceId)) {
        std::cerr << "Rejected recursive clip anchor change for clip " << clipId << std::endl;
        return;
    }

    if (!targetTrack->clipManager().setClipAnchor(clipId, anchor, appModel.sampleRate())) {
        std::cerr << "Failed to apply clip anchor change for clip " << clipId << std::endl;
        return;
    }
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::updateClipName(int32_t trackIndex, int32_t clipId, const std::string& name) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    tracks[trackIndex]->clipManager().setClipName(clipId, name);
    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::changeClipFile(int32_t trackIndex, int32_t clipId) {
    auto requestChange = [this, trackIndex, clipId](const std::string& selectedFile) {
        auto& appModel = uapmd::AppModel::instance();
        auto tracks = appModel.getTimelineTracks();

        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
            return;

        auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
        if (!reader) {
            platformError("Load Failed",
                          "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG");
            return;
        }

        auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
        if (!clip) {
            platformError("Error", "Could not find clip");
            return;
        }

        int32_t sourceNodeId = clip->sourceNodeInstanceId;

        auto sourceNode = std::make_unique<uapmd::AudioFileSourceNode>(
            sourceNodeId,
            std::move(reader),
            static_cast<double>(appModel.sampleRate()),
            resolveAudioWarpPoints(
                *clip,
                clip->audioWarps,
                buildClipReferenceMap(tracks),
                appModel.masterTrackMarkers())
        );

        int64_t durationSamples = sourceNode->totalLength();

        if (!tracks[trackIndex]->replaceClipSourceNode(clipId, std::move(sourceNode))) {
            platformError("Replace Failed", "Could not replace clip source node");
            return;
        }

        tracks[trackIndex]->clipManager().setClipFilepath(clipId, selectedFile);
        tracks[trackIndex]->clipManager().resizeClip(clipId, durationSamples);
        refreshSequenceEditorForTrack(trackIndex);
    };

    std::vector<uapmd::DocumentFilter> filters{
        {"Audio Files", {}, {"*.wav", "*.flac", "*.ogg"}},
        {"WAV Files",   {}, {"*.wav"}},
        {"FLAC Files",  {}, {"*.flac"}},
        {"OGG Files",   {}, {"*.ogg"}},
        {"All Files",   {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [requestChange = std::move(requestChange)](uapmd::DocumentPickResult result) mutable {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [requestChange = std::move(requestChange)](const std::filesystem::path& resolved) mutable {
                        requestChange(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Open Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::moveClipAbsolute(int32_t trackIndex, int32_t clipId, double seconds) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    double sr = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    tracks[trackIndex]->clipManager().setClipAnchor(
        clipId,
        uapmd::TimeReference::fromContainerStart({}, seconds),
        static_cast<int32_t>(sr));
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::showMidiClipDump(int32_t trackIndex, int32_t clipId) {
    midiDumpWindow_.showClipDump(buildMidiClipDumpData(trackIndex, clipId));
}

void TimelineEditor::showAudioClipEvents(int32_t trackIndex, int32_t clipId) {
    audioEventListEditor_.showClip(buildAudioEventListData(trackIndex, clipId));
}

void TimelineEditor::showMasterMarkerEditor() {
    audioEventListEditor_.showClip(buildAudioEventListData(uapmd::kMasterTrackIndex, kMasterTrackClipId));
}

void TimelineEditor::showPianoRoll(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;
    auto* track = tracks[trackIndex];
    if (!track)
        return;
    const auto* clipData = track->clipManager().getClip(clipId);
    if (!clipData)
        return;
    if (clipData->clipType != uapmd::ClipType::Midi)
        return;

    // createMidiClipPreview will determine duration from MIDI events;
    // pass 0.0 as fallback — it's only used when the clip has no events.
    const double fallbackDuration = 0.0;
    auto preview = createMidiClipPreview(trackIndex, *clipData, fallbackDuration);
    std::string clipName = clipData->name.empty()
        ? std::format("Clip {}", clipId) : clipData->name;
    pianoRollEditor_.showClip(trackIndex, clipId, clipName, std::move(preview));
}

void TimelineEditor::showMasterMetaDump() {
    midiDumpWindow_.showClipDump(buildMasterMetaDumpData());
}

MidiDumpWindow::ClipDumpData TimelineEditor::buildMidiClipDumpData(int32_t trackIndex, int32_t clipId) {
    MidiDumpWindow::ClipDumpData dump;
    dump.trackIndex = trackIndex;
    dump.clipId = clipId;

    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        dump.error = "Invalid track index";
        return dump;
    }

    auto* track = tracks[trackIndex];
    if (!track) {
        dump.error = "Track is unavailable";
        return dump;
    }

    auto* clip = track->clipManager().getClip(clipId);
    if (!clip) {
        dump.error = "Clip not found";
        return dump;
    }

    dump.clipName = clip->name.empty() ? std::format("Clip {}", clip->clipId) : clip->name;
    dump.filepath = clip->filepath;
    if (clip->clipType != uapmd::ClipType::Midi) {
        dump.error = "Selected clip is not a MIDI clip";
        return dump;
    }

    auto sourceNode = track->getSourceNode(clip->sourceNodeInstanceId);
    if (!sourceNode) {
        dump.error = "Source node not found";
        return dump;
    }

    auto* midiSourceNode = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
    if (!midiSourceNode) {
        dump.error = "Source node is not a MIDI clip source";
        return dump;
    }

    if (clip->filepath.empty()) {
        dump.fileLabel = "(in-memory)";
    } else {
        std::filesystem::path clipPath(clip->filepath);
        dump.fileLabel = clipPath.filename().string();
        if (dump.fileLabel.empty())
            dump.fileLabel = clip->filepath;
    }

    dump.tickResolution = midiSourceNode->tickResolution();
    dump.tempo = midiSourceNode->clipTempo();

    uapmd::MidiClipReader::ClipInfo clipInfo;
    clipInfo.success = true;
    clipInfo.ump_data = midiSourceNode->umpEvents();
    clipInfo.ump_tick_timestamps = midiSourceNode->eventTimestampsTicks();
    clipInfo.tick_resolution = dump.tickResolution;
    clipInfo.tempo = dump.tempo;

    dump.events = buildMidiDumpRows(clipInfo, dump.tickResolution, dump.tempo);
    dump.success = true;
    dump.error.clear();
    return dump;
}

MidiDumpWindow::ClipDumpData TimelineEditor::buildMasterMetaDumpData() {
    MidiDumpWindow::ClipDumpData dump;
    dump.trackIndex = -1;
    dump.clipId = -1;
    dump.isMasterTrack = true;
    dump.clipName = "Master Track Meta Events";
    dump.fileLabel = "Aggregated SMF meta events";

    std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot> snapshot = masterTrackSnapshot_;
    if (!snapshot) {
        snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
            uapmd::AppModel::instance().buildMasterTrackSnapshot());
    }

    if (!snapshot || snapshot->empty()) {
        dump.success = false;
        dump.error = "No tempo or time signature events are available.";
        return dump;
    }

    struct MetaRow {
        double time{0.0};
        MidiDumpWindow::EventRow row;
    };

    std::vector<MetaRow> rows;
    rows.reserve(snapshot->tempoPoints.size() + snapshot->timeSignaturePoints.size());

    for (const auto& point : snapshot->tempoPoints) {
        if (point.bpm <= 0.0)
            continue;
        MidiDumpWindow::EventRow row;
        row.timeSeconds = point.timeSeconds;
        row.tickPosition = point.tickPosition;
        row.timeLabel = std::format("{:.6f}s [{}]", row.timeSeconds, row.tickPosition);

        double clampedBpm = std::clamp(point.bpm, 1.0, 1000.0);
        uint64_t usec = static_cast<uint64_t>(std::llround(60000000.0 / clampedBpm));
        if (usec > 0xFFFFFFu)
            usec = 0xFFFFFFu;

        uint8_t b0 = static_cast<uint8_t>((usec >> 16) & 0xFF);
        uint8_t b1 = static_cast<uint8_t>((usec >> 8) & 0xFF);
        uint8_t b2 = static_cast<uint8_t>(usec & 0xFF);
        row.hexBytes = std::format(
            "FF 51 03 {:02X} {:02X} {:02X}    (Tempo {:.2f} BPM)",
            b0, b1, b2, clampedBpm
        );
        rows.push_back(MetaRow{row.timeSeconds, row});
    }

    for (const auto& point : snapshot->timeSignaturePoints) {
        MidiDumpWindow::EventRow row;
        row.timeSeconds = point.timeSeconds;
        row.tickPosition = point.tickPosition;
        row.timeLabel = std::format("{:.6f}s [{}]", row.timeSeconds, row.tickPosition);

        uint8_t denominator = std::max<uint8_t>(1, point.signature.denominator);
        uint8_t exponent = 0;
        uint8_t denomValue = denominator;
        while (denomValue > 1 && exponent < 7) {
            denomValue >>= 1;
            ++exponent;
        }

        row.hexBytes = std::format(
            "FF 58 04 {:02X} {:02X} {:02X} {:02X}    (Time Sig {}/{} )",
            point.signature.numerator,
            exponent,
            point.signature.clocksPerClick,
            point.signature.thirtySecondsPerQuarter,
            point.signature.numerator,
            denominator
        );
        rows.push_back(MetaRow{row.timeSeconds, row});
    }

    std::sort(rows.begin(), rows.end(),
        [](const MetaRow& a, const MetaRow& b) {
            return a.time < b.time;
        });

    dump.events.reserve(rows.size());
    uint64_t prevTick = 0;
    for (auto& row : rows) {
        row.row.deltaTicks = row.row.tickPosition > prevTick ? row.row.tickPosition - prevTick : 0;
        prevTick = row.row.tickPosition;
        dump.events.push_back(std::move(row.row));
    }

    dump.success = true;
    dump.error.clear();
    return dump;
}

AudioEventListEditor::ClipData TimelineEditor::buildAudioEventListData(int32_t trackIndex, int32_t clipId) {
    AudioEventListEditor::ClipData data;
    data.trackIndex = trackIndex;
    data.clipId = clipId;

    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex == uapmd::kMasterTrackIndex) {
        data.markerOnly = true;
        data.clipName = "Markers";
        data.clipReferenceId = "master_track";
        data.sampleRate = appModel.sampleRate();
        if (masterTrackSnapshot_)
            data.durationSamples = static_cast<int64_t>(std::llround(std::max(0.0, masterTrackSnapshot_->maxTimeSeconds) * data.sampleRate));
        data.markers = audioEventListEditor_.draftMasterMarkers().empty()
            ? appModel.masterTrackMarkers()
            : audioEventListEditor_.draftMasterMarkers();
        data.fileLabel = "Master Track";
        data.success = true;
        return data;
    }

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        data.error = "Invalid track index";
        return data;
    }

    auto* track = tracks[trackIndex];
    if (!track) {
        data.error = "Track is unavailable";
        return data;
    }

    auto* clip = track->clipManager().getClip(clipId);
    if (!clip) {
        data.error = "Clip not found";
        return data;
    }
    data.clipReferenceId = clip->referenceId;

    data.clipName = clip->name.empty() ? std::format("Clip {}", clip->clipId) : clip->name;
    data.filepath = clip->filepath;
    data.sampleRate = appModel.sampleRate();
    data.durationSamples = clip->durationSamples;
    if (clip->clipType != uapmd::ClipType::Audio) {
        data.error = "Selected clip is not an audio clip";
        return data;
    }

    data.markers = clip->markers;
    data.audioWarps = clip->audioWarps;
    const auto draftMarkers = audioEventListEditor_.draftMarkersByClipReference();
    if (auto it = draftMarkers.find(clip->referenceId); it != draftMarkers.end())
        data.markers = it->second;
    data.externalReferenceOptions = ::uapmd::gui::buildExternalReferenceOptions(
        trackIndex,
        clipId,
        audioEventListEditor_,
        tracks,
        appModel.masterTrackMarkers()
    );
    if (clip->filepath.empty()) {
        data.fileLabel = "(missing file path)";
    } else {
        std::filesystem::path clipPath(clip->filepath);
        data.fileLabel = clipPath.filename().string();
        if (data.fileLabel.empty())
            data.fileLabel = clip->filepath;
    }

    data.success = true;
    return data;
}

bool TimelineEditor::applyMidiClipEdits(const MidiDumpWindow::EditPayload& payload, std::string& error) {
    if (payload.trackIndex < 0) {
        error = "Editing master track meta events is not supported.";
        return false;
    }

    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (payload.trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[payload.trackIndex]) {
        error = "Track is unavailable.";
        return false;
    }

    auto& track = tracks[payload.trackIndex];
    auto* clip = track->clipManager().getClip(payload.clipId);
    if (!clip) {
        error = "Clip not found.";
        return false;
    }
    if (clip->clipType != uapmd::ClipType::Midi) {
        error = "Selected clip is not a MIDI clip.";
        return false;
    }

    auto sourceNode = track->getSourceNode(clip->sourceNodeInstanceId);
    auto midiNode = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(sourceNode);
    if (!midiNode) {
        error = "MIDI source node is not available.";
        return false;
    }

    std::vector<uapmd_ump_t> newEvents;
    std::vector<uint64_t> newTicks;
    size_t totalWords = 0;
    for (const auto& evt : payload.events) {
        if (evt.words.empty()) {
            error = "Event has empty data.";
            return false;
        }
        totalWords += evt.words.size();
    }
    newEvents.reserve(totalWords);
    newTicks.reserve(totalWords);

    uint64_t prevTick = 0;
    for (const auto& evt : payload.events) {
        if (!newEvents.empty() && evt.tickPosition < prevTick) {
            error = "Events must be ordered by tick.";
            return false;
        }
        prevTick = evt.tickPosition;
        for (auto word : evt.words) {
            newEvents.push_back(static_cast<uapmd_ump_t>(word));
            newTicks.push_back(evt.tickPosition);
        }
    }

    auto tempoChanges = midiNode->tempoChanges();
    auto timeSignatureChanges = midiNode->timeSignatureChanges();
    double clipTempo = midiNode->clipTempo();
    uint32_t tickResolution = clip->tickResolution > 0 ? clip->tickResolution : midiNode->tickResolution();
    auto newNode = std::make_unique<uapmd::MidiClipSourceNode>(
        midiNode->instanceId(),
        std::move(newEvents),
        std::move(newTicks),
        tickResolution,
        clipTempo,
        static_cast<double>(appModel.sampleRate()),
        tempoChanges,
        timeSignatureChanges
    );

    const int64_t newDuration = newNode->totalLength();
    if (!track->replaceClipSourceNode(clip->clipId, std::move(newNode))) {
        error = "Failed to replace MIDI clip data.";
        return false;
    }

    track->clipManager().resizeClip(clip->clipId, newDuration);
    refreshSequenceEditorForTrack(payload.trackIndex);
    return true;
}

bool TimelineEditor::applyAudioClipEdits(const AudioEventListEditor::EditPayload& payload, std::string& error) {
    auto& appModel = uapmd::AppModel::instance();
    if (payload.trackIndex == uapmd::kMasterTrackIndex) {
        appModel.setMasterTrackMarkers(payload.markers);
        invalidateMasterTrackSnapshot();
        refreshAllSequenceEditorTracks();
        return true;
    }

    auto tracks = appModel.getTimelineTracks();
    if (payload.trackIndex < 0 || payload.trackIndex >= static_cast<int32_t>(tracks.size()) ||
        !tracks[payload.trackIndex]) {
        error = "Track unavailable.";
        return false;
    }

    auto& track = tracks[payload.trackIndex];
    auto* clip = track->clipManager().getClip(payload.clipId);
    if (!clip) {
        error = "Clip not found.";
        return false;
    }
    if (clip->clipType != uapmd::ClipType::Audio) {
        error = "Selected clip is not an audio clip.";
        return false;
    }
    if (clip->filepath.empty()) {
        error = "Audio clip has no source file path.";
        return false;
    }
    const int64_t authoredDuration = clip->durationSamples;
    const bool preserveClipDuration = std::ranges::any_of(payload.markers, [&](const auto& marker) {
            return referencesThisClipEnd(marker, clip->referenceId);
        }) || std::ranges::any_of(payload.audioWarps, [&](const auto& warp) {
            return referencesThisClipEnd(warp, clip->referenceId);
        });

    auto reader = uapmd::createAudioFileReaderFromPath(clip->filepath);
    if (!reader) {
        error = "Could not reopen the audio file for warp rebuild.";
        return false;
    }

    auto clipLookup = buildClipReferenceMap(tracks);
    auto targetClip = *clip;
    targetClip.markers = payload.markers;
    clipLookup[targetClip.referenceId] = targetClip;
    auto resolvedWarps = resolveAudioWarpPoints(targetClip, payload.audioWarps, clipLookup, appModel.masterTrackMarkers());
    auto newNode = std::make_unique<uapmd::AudioFileSourceNode>(
        clip->sourceNodeInstanceId,
        std::move(reader),
        static_cast<double>(appModel.sampleRate()),
        resolvedWarps
    );

    if (!track->clipManager().setClipMarkers(payload.clipId, payload.markers)) {
        error = "Failed to update clip markers.";
        return false;
    }
    if (!track->clipManager().setAudioWarps(payload.clipId, payload.audioWarps)) {
        error = "Failed to update audio warp points.";
        return false;
    }
    if (!track->replaceClipSourceNode(payload.clipId, std::move(newNode))) {
        error = "Failed to rebuild warped audio source.";
        return false;
    }
    if (preserveClipDuration)
        track->clipManager().resizeClip(payload.clipId, authoredDuration);

    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
    return true;
}

bool TimelineEditor::applyPianoRollEdits(int32_t trackIndex, int32_t clipId,
                                          std::vector<uapmd_ump_t> newUmpEvents,
                                          std::vector<uint64_t>    newTickTimestamps,
                                          std::string&             error) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) ||
            !tracks[trackIndex]) {
        error = "Track unavailable.";
        return false;
    }
    auto& track = tracks[trackIndex];
    auto* clip = track->clipManager().getClip(clipId);
    if (!clip) {
        error = "Clip not found.";
        return false;
    }
    auto sourceNode = track->getSourceNode(clip->sourceNodeInstanceId);
    auto midiNode = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(sourceNode);
    if (!midiNode) {
        error = "MIDI source node unavailable.";
        return false;
    }
    uint32_t tickResolution = clip->tickResolution > 0
                              ? clip->tickResolution
                              : midiNode->tickResolution();
    auto newNode = std::make_unique<uapmd::MidiClipSourceNode>(
        midiNode->instanceId(),
        std::move(newUmpEvents),
        std::move(newTickTimestamps),
        tickResolution,
        midiNode->clipTempo(),
        static_cast<double>(appModel.sampleRate()),
        midiNode->tempoChanges(),
        midiNode->timeSignatureChanges()
    );
    const int64_t newDuration = newNode->totalLength();
    if (!track->replaceClipSourceNode(clipId, std::move(newNode))) {
        error = "Failed to replace MIDI clip data.";
        return false;
    }
    track->clipManager().resizeClip(clipId, newDuration);
    refreshSequenceEditorForTrack(trackIndex);
    return true;
}

// ── Plugin parameter query ────────────────────────────────────────────────────

std::vector<PianoRollEditor::PluginParamEntry>
TimelineEditor::getPluginParametersForTrack(int32_t trackIndex) const {
    std::vector<PianoRollEditor::PluginParamEntry> result;
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto tracksRef = sequencer.engine()->tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracksRef.size()) || !tracksRef[trackIndex])
        return result;
    for (int32_t instanceId : tracksRef[trackIndex]->orderedInstanceIds()) {
        auto* pal = sequencer.engine()->getPluginInstance(instanceId);
        if (!pal) continue;
        PianoRollEditor::PluginParamEntry entry;
        entry.instanceId = instanceId;
        entry.pluginName = pal->displayName();
        entry.group = tracksRef[trackIndex]->getInstanceGroup(instanceId);
        for (const auto& p : pal->parameterMetadataList()) {
            if (!p.automatable) continue;
            if (p.index >= 16384u) continue; // exceeds 14-bit addressable range
            PianoRollEditor::PluginParamEntry::Param param;
            param.nrpnIndex = static_cast<uint16_t>(
                ((p.index / 0x80u) << 7u) | (p.index % 0x80u));
            param.path = p.path;
            param.name = p.name;
            entry.params.push_back(std::move(param));
        }
        if (!entry.params.empty())
            result.push_back(std::move(entry));
    }
    return result;
}

// ── Per-type clip import helpers ──────────────────────────────────────────────

void TimelineEditor::addBlankMidi2ClipToTrack(int32_t trackIndex) {
    uapmd::TimelinePosition position;
    position.samples      = 0;
    position.legacy_beats = 0.0;
    auto& appModel = uapmd::AppModel::instance();
    auto result = appModel.addMidiClipToTrack(
        trackIndex, position,
        {},        // empty UMP events
        {},        // empty tick timestamps
        480,       // standard PPQ
        120.0,     // default BPM
        {},        // no tempo changes
        {},        // no time-signature changes
        "New Clip"
    );
    if (!result.success)
        platformError("New Clip Failed", "Could not create blank MIDI clip: " + result.error);
    else
        refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addBlankMidi2ClipToTrackAtPosition(int32_t trackIndex, double positionSeconds) {
    auto& appModel = uapmd::AppModel::instance();
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    uapmd::TimelinePosition position;
    position.samples      = static_cast<int64_t>(std::llround(std::max(0.0, positionSeconds) * sampleRate));
    position.legacy_beats = 0.0;
    auto result = appModel.addMidiClipToTrack(
        trackIndex, position,
        {},        // empty UMP events
        {},        // empty tick timestamps
        480,       // standard PPQ
        120.0,     // default BPM
        {},        // no tempo changes
        {},        // no time-signature changes
        "New Clip"
    );
    if (!result.success)
        platformError("New Clip Failed", "Could not create blank MIDI clip: " + result.error);
    else
        refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addBlankMidiClipInRange(int32_t trackIndex, double startSeconds, double endSeconds) {
    auto& appModel = uapmd::AppModel::instance();
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    uapmd::TimelinePosition position;
    position.samples = static_cast<int64_t>(std::llround(std::max(0.0, startSeconds) * sampleRate));
    auto result = appModel.addMidiClipToTrack(
        trackIndex, position,
        {},        // empty UMP events
        {},        // empty tick timestamps
        480,       // standard PPQ
        120.0,     // default BPM
        {},        // no tempo changes
        {},        // no time-signature changes
        "New Clip"
    );
    if (!result.success) {
        platformError("New Clip Failed", "Could not create blank MIDI clip: " + result.error);
        return;
    }

    // Resize to the selected range's length (blank clips default to a small fixed length).
    const int64_t durationSamples = static_cast<int64_t>(std::llround(std::max(0.0, endSeconds - startSeconds) * sampleRate));
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()) && tracks[trackIndex])
        tracks[trackIndex]->clipManager().resizeClip(result.clipId, std::max<int64_t>(1, durationSamples));

    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addEmptyAudioClipInRange(int32_t trackIndex, double startSeconds, double endSeconds) {
    if (trackIndex == uapmd::kMasterTrackIndex) {
        platformError("Unsupported", "The master track only accepts MIDI/SMF clips.");
        return;
    }
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex])
        return;

    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    const uint64_t numFrames = static_cast<uint64_t>(
        std::max<int64_t>(1, std::llround(std::max(0.0, endSeconds - startSeconds) * sampleRate)));
    const uint32_t numChannels = std::max<uint32_t>(1, tracks[trackIndex]->channelCount());
    auto reader = std::make_unique<uapmd::SilentAudioFileReader>(
        numFrames, numChannels, static_cast<uint32_t>(appModel.sampleRate()));

    uapmd::TimelinePosition position;
    position.samples = static_cast<int64_t>(std::llround(std::max(0.0, startSeconds) * sampleRate));
    auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), "");
    if (!result.success)
        platformError("Add Clip Failed", result.error);
    else
        refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addAudioClipToTrack(int32_t trackIndex, double positionSeconds) {
    if (trackIndex == uapmd::kMasterTrackIndex) {
        platformError("Unsupported", "The master track only accepts MIDI/SMF clips.");
        return;
    }
    std::vector<uapmd::DocumentFilter> filters{
        {"Audio Files", {}, {"*.wav", "*.flac", "*.ogg"}},
        {"WAV Files",   {}, {"*.wav"}},
        {"FLAC Files",  {}, {"*.flac"}},
        {"OGG Files",   {}, {"*.ogg"}},
        {"All Files",   {}, {"*"}}
    };
    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters, false,
            [this, trackIndex, positionSeconds](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty()) return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this, trackIndex, positionSeconds](const std::filesystem::path& resolved) {
                        auto& appModel = uapmd::AppModel::instance();
                        const double sr = std::max(1.0, static_cast<double>(appModel.sampleRate()));
                        auto reader = uapmd::createAudioFileReaderFromPath(resolved.string());
                        if (!reader) {
                            platformError("Load Failed",
                                          "Could not load audio file: " + resolved.string());
                            return;
                        }
                        uapmd::TimelinePosition pos;
                        pos.samples      = static_cast<int64_t>(std::llround(std::max(0.0, positionSeconds) * sr));
                        pos.legacy_beats = 0.0;
                        auto r = appModel.addClipToTrack(trackIndex, pos, std::move(reader), resolved.string());
                        if (!r.success)
                            platformError("Add Clip Failed", r.error);
                        else
                            refreshSequenceEditorForTrack(trackIndex);
                    },
                    [](const std::string& error) { platformError("Open Failed", error); });
            }
        );
    }
}

void TimelineEditor::addSmfClipToTrack(int32_t trackIndex, double positionSeconds) {
    std::vector<uapmd::DocumentFilter> filters{
        {"SMF Files", {}, {"*.mid", "*.midi", "*.smf"}},
        {"All Files", {}, {"*"}}
    };
    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters, false,
            [this, trackIndex, positionSeconds](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty()) return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this, trackIndex, positionSeconds](const std::filesystem::path& resolved) {
                        auto& appModel = uapmd::AppModel::instance();
                        const double sr = std::max(1.0, static_cast<double>(appModel.sampleRate()));
                        uapmd::TimelinePosition pos;
                        pos.samples      = static_cast<int64_t>(std::llround(std::max(0.0, positionSeconds) * sr));
                        pos.legacy_beats = 0.0;
                        auto r = appModel.addClipToTrack(trackIndex, pos, nullptr, resolved.string());
                        if (!r.success)
                            platformError("Add Clip Failed", r.error);
                        else
                            refreshSequenceEditorForTrack(trackIndex);
                    },
                    [](const std::string& error) { platformError("Open Failed", error); });
            }
        );
    }
}

void TimelineEditor::addSmf2ClipToTrack(int32_t trackIndex) {
    std::vector<uapmd::DocumentFilter> filters{
        {"MIDI 2.0 Files", {}, {"*.midi2"}},
        {"All Files",      {}, {"*"}}
    };
    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters, false,
            [this, trackIndex](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty()) return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this, trackIndex](const std::filesystem::path& resolved) {
                        uapmd::TimelinePosition pos;
                        pos.samples = 0;
                        pos.legacy_beats = 0.0;
                        auto r = uapmd::AppModel::instance().addClipToTrack(
                            trackIndex, pos, nullptr, resolved.string());
                        if (!r.success)
                            platformError("Add Clip Failed", r.error);
                        else
                            refreshSequenceEditorForTrack(trackIndex);
                    },
                    [](const std::string& error) { platformError("Open Failed", error); });
            }
        );
    }
}

void TimelineEditor::importMidiTracksWithPicker() {
    std::vector<uapmd::DocumentFilter> filters{
        {"MIDI Files", {}, {"*.mid", "*.midi", "*.smf"}},
        {"All Files",  {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [this](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this](const std::filesystem::path& resolved) {
                        importMidiTracks(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Import Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::importMidiTracks(const std::string& filepath) {
    auto& appModel = uapmd::AppModel::instance();
    auto result = appModel.importMidiTracksFromFile(filepath);

    for (const auto& track : result.importedTracks)
        if (track.success)
            refreshSequenceEditorForTrack(track.trackIndex);

    invalidateMasterTrackSnapshot();

    if (!result.success) {
        std::string message = result.error.empty() ? "No MIDI tracks were imported." : result.error;
        if (!result.warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : result.warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
    }
}

void TimelineEditor::applyAudioImportResult(uapmd::import::AudioImportResult result) {
    auto warnings = result.warnings;
    if (result.canceled)
        return;
    if (!result.success) {
        std::string message = result.error.empty()
            ? "Failed to import audio stems."
            : result.error;
        if (!warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
        return;
    }

    auto& appModel = uapmd::AppModel::instance();
    size_t importedCount = 0;

    for (const auto& stem : result.stems) {
        int32_t newTrackIndex = appModel.addTrack();
        if (newTrackIndex < 0) {
            warnings.push_back(std::format("{}: Failed to create track", stem.clipDisplayName));
            continue;
        }

        auto reader = uapmd::createAudioFileReaderFromPath(stem.filepath.string());
        if (!reader) {
            warnings.push_back(std::format("{}: Failed to open stem audio", stem.clipDisplayName));
            continue;
        }

        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;

        auto clipResult = appModel.addClipToTrack(
            newTrackIndex,
            position,
            std::move(reader),
            stem.filepath.string()
        );

        if (!clipResult.success) {
            warnings.push_back(std::format("{}: {}", stem.clipDisplayName, clipResult.error));
            continue;
        }

        auto tracks = appModel.getTimelineTracks();
        if (newTrackIndex >= 0 && newTrackIndex < static_cast<int32_t>(tracks.size())) {
            auto& clipManager = tracks[newTrackIndex]->clipManager();
            clipManager.setClipName(clipResult.clipId, stem.clipDisplayName);
            clipManager.setClipNeedsFileSave(clipResult.clipId, true);
        }

        refreshSequenceEditorForTrack(newTrackIndex);
        ++importedCount;
    }

    if (importedCount == 0) {
        std::string message = "No stems were imported.";
        if (!warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
    }
}

}  // namespace uapmd::gui
