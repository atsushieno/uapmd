#include <algorithm>
#include <charconv>
#include <string_view>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd {
namespace {

constexpr std::string_view kManifestHeader = "uapmd-track-freezing-v1";

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

} // namespace

FrozenTrackManagerProjectSerializationExtension::FrozenTrackManagerProjectSerializationExtension(
    FrozenTrackManager& manager)
    : manager_(manager) {
}

std::string_view FrozenTrackManagerProjectSerializationExtension::extensionId() const {
    return manager_.extensionId();
}

bool FrozenTrackManagerProjectSerializationExtension::saveProjectExtensionData(
    ProjectSerializationWriteContext& context, std::string& error) {
    return manager_.saveProjectExtensionData(context, error);
}

bool FrozenTrackManagerProjectSerializationExtension::loadProjectExtensionData(
    ProjectSerializationReadContext& context, std::string& error) {
    return manager_.loadProjectExtensionData(context, error);
}

FrozenTrackAudioProcessorExtension::FrozenTrackAudioProcessorExtension(FrozenTrackManager& manager)
    : manager_(manager) {
}

bool FrozenTrackAudioProcessorExtension::shouldProcessAudio(
    SequencerEngine& engine,
    uapmd_track_index_t trackIndex,
    SequencerTrack& track,
    AudioProcessContext& context) {
    return manager_.shouldProcessAudio(engine, trackIndex, track, context);
}

void FrozenTrackAudioProcessorExtension::processAudio(
    SequencerEngine& engine,
    uapmd_track_index_t trackIndex,
    SequencerTrack& track,
    AudioProcessContext& context) {
    manager_.processAudio(engine, trackIndex, track, context);
}

FrozenTrackManager::FrozenTrackManager(TimelineFacade& timeline)
    : timeline_(timeline)
    , project_serialization_extension_(std::make_unique<FrozenTrackManagerProjectSerializationExtension>(*this))
    , audio_processor_extension_(std::make_unique<FrozenTrackAudioProcessorExtension>(*this)) {
}

FrozenTrackManager::~FrozenTrackManager() = default;

FrozenTrackManagerProjectSerializationExtension& FrozenTrackManager::projectSerializationExtension() {
    return *project_serialization_extension_;
}

FrozenTrackAudioProcessorExtension& FrozenTrackManager::audioProcessorExtension() {
    return *audio_processor_extension_;
}

std::string_view FrozenTrackManager::extensionId() const {
    return kExtensionId;
}

bool FrozenTrackManager::shouldProcessAudio(
    SequencerEngine&, uapmd_track_index_t, SequencerTrack&, AudioProcessContext&) {
    return false;
}

void FrozenTrackManager::processAudio(
    SequencerEngine&, uapmd_track_index_t, SequencerTrack&, AudioProcessContext&) {
}

int FrozenTrackManager::autoFreezeMinutes() const {
    std::lock_guard lock(mutex_);
    return auto_freeze_minutes_;
}

bool FrozenTrackManager::setAutoFreezeMinutes(int minutes) {
    const int clamped = std::clamp(minutes, 1, 20);
    std::lock_guard lock(mutex_);
    if (auto_freeze_minutes_ == clamped)
        return false;
    auto_freeze_minutes_ = clamped;
    return true;
}

FrozenTrackManager::FreezePolicy FrozenTrackManager::freezePolicyForTrack(int32_t trackIndex) const {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return FreezePolicy::Off;

    std::lock_guard lock(mutex_);
    if (auto it = policies_by_track_reference_.find(referenceId); it != policies_by_track_reference_.end())
        return it->second;
    return FreezePolicy::Off;
}

bool FrozenTrackManager::setFreezePolicyForTrack(int32_t trackIndex, FreezePolicy policy) {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return false;

    std::lock_guard lock(mutex_);
    const auto existing = policies_by_track_reference_.find(referenceId);
    if (policy == FreezePolicy::Off) {
        if (existing == policies_by_track_reference_.end())
            return false;
        policies_by_track_reference_.erase(existing);
        return true;
    }
    if (existing != policies_by_track_reference_.end() && existing->second == policy)
        return false;
    policies_by_track_reference_[referenceId] = policy;
    return true;
}

bool FrozenTrackManager::unfreezeTrack(int32_t trackIndex) {
    if (freezePolicyForTrack(trackIndex) != FreezePolicy::On)
        return false;
    return setFreezePolicyForTrack(trackIndex, FreezePolicy::Off);
}

bool FrozenTrackManager::saveProjectExtensionData(ProjectSerializationWriteContext& context, std::string& error) {
    std::unordered_map<std::string, FreezePolicy> policies;
    int autoFreezeMinutes = kDefaultAutoFreezeMinutes;
    {
        std::lock_guard lock(mutex_);
        policies = policies_by_track_reference_;
        autoFreezeMinutes = auto_freeze_minutes_;
    }

    std::string manifest(kManifestHeader);
    manifest += "\nauto-freeze-minutes=" + std::to_string(autoFreezeMinutes) + "\n";
    for (const auto& [referenceId, policy] : policies) {
        if (referenceId.empty() || policy == FreezePolicy::Off)
            continue;
        manifest += "track." + referenceId + "=" + policyName(policy) + "\n";
    }

    return context.writeExtensionFile(
        extensionId(), kManifestPath,
        std::vector<uint8_t>(manifest.begin(), manifest.end()), error);
}

bool FrozenTrackManager::loadProjectExtensionData(ProjectSerializationReadContext& context, std::string& error) {
    std::string readError;
    auto bytes = context.readExtensionFile(extensionId(), kManifestPath, readError);
    if (!bytes) {
        std::lock_guard lock(mutex_);
        auto_freeze_minutes_ = kDefaultAutoFreezeMinutes;
        policies_by_track_reference_.clear();
        error.clear();
        return true;
    }

    std::string_view manifest(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    const auto newline = manifest.find('\n');
    if (manifest.substr(0, newline) != kManifestHeader) {
        error = "Unsupported track-freezing manifest version.";
        return false;
    }

    int autoFreezeMinutes = kDefaultAutoFreezeMinutes;
    std::unordered_map<std::string, FreezePolicy> policies;
    size_t offset = newline == std::string_view::npos ? manifest.size() : newline + 1;
    while (offset < manifest.size()) {
        const auto lineEnd = manifest.find('\n', offset);
        const auto line = trim(manifest.substr(offset, lineEnd - offset));
        offset = lineEnd == std::string_view::npos ? manifest.size() : lineEnd + 1;
        if (line.empty())
            continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key == "auto-freeze-minutes") {
            int parsed = kDefaultAutoFreezeMinutes;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec == std::errc{} && ptr == value.data() + value.size())
                autoFreezeMinutes = std::clamp(parsed, 1, 20);
            continue;
        }
        constexpr std::string_view trackPrefix{"track."};
        if (key.starts_with(trackPrefix)) {
            const auto referenceId = key.substr(trackPrefix.size());
            if (auto policy = parsePolicy(value); policy && !referenceId.empty())
                policies.emplace(referenceId, *policy);
        }
    }

    std::lock_guard lock(mutex_);
    auto_freeze_minutes_ = autoFreezeMinutes;
    policies_by_track_reference_ = std::move(policies);
    return true;
}

std::string FrozenTrackManager::trackReferenceIdForIndex(int32_t trackIndex) const {
    if (trackIndex < 0)
        return {};
    auto tracks = timeline_.tracks();
    if (static_cast<size_t>(trackIndex) >= tracks.size() || !tracks[static_cast<size_t>(trackIndex)])
        return {};
    return tracks[static_cast<size_t>(trackIndex)]->referenceId();
}

std::string FrozenTrackManager::policyName(FreezePolicy policy) {
    switch (policy) {
        case FreezePolicy::Auto: return "auto";
        case FreezePolicy::On: return "on";
        case FreezePolicy::Off:
        default: return "off";
    }
}

std::optional<FrozenTrackManager::FreezePolicy> FrozenTrackManager::parsePolicy(std::string_view value) {
    if (value == "auto")
        return FreezePolicy::Auto;
    if (value == "on")
        return FreezePolicy::On;
    if (value == "off")
        return FreezePolicy::Off;
    return std::nullopt;
}

} // namespace uapmd
