#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uapmd {

class SequencerEngine;

enum class OfflineInfiniteTailPolicy {
    USE_GUARD_AND_SILENCE_STOP = 0,
    LATENCY_FALLBACK = 1,
};

struct OfflineRenderSettings {
    std::filesystem::path outputPath;
    double startSeconds{0.0};
    std::optional<double> endSeconds;
    bool useContentFallback{false};
    bool contentBoundsValid{false};
    double contentStartSeconds{0.0};
    double contentEndSeconds{0.0};
    double tailSeconds{0.0};
    bool enableSilenceStop{false};
    double silenceDurationSeconds{0.0};
    double silenceThresholdDb{-80.0};
    OfflineInfiniteTailPolicy infiniteTailPolicy{OfflineInfiniteTailPolicy::USE_GUARD_AND_SILENCE_STOP};
    int32_t sampleRate{48000};
    uint32_t bufferSize{1024};
    uint32_t outputChannels{2};
    uint32_t umpBufferSize{65536};
};

struct OfflineRenderProgress {
    double progress{0.0};
    double renderedSeconds{0.0};
    double totalSeconds{0.0};
    int64_t renderedFrames{0};
    int64_t totalFrames{0};
};

struct OfflineRenderCallbacks {
    std::function<void(const OfflineRenderProgress&)> onProgress;
    std::function<bool()> shouldCancel;
};

struct OfflineRenderResult {
    bool success{false};
    bool canceled{false};
    double renderedSeconds{0.0};
    std::string errorMessage;
};

struct OfflineTrackRenderSettings {
    int32_t trackIndex{0};
    int64_t startSample{0};
    int64_t endSample{0};
    int32_t sampleRate{48000};
    uint32_t bufferSize{1024};
    uint32_t umpBufferSize{65536};
    uint64_t maximumBytes{512ULL * 1024ULL * 1024ULL};
};

struct OfflineTrackRenderResult {
    bool success{false};
    bool canceled{false};
    int64_t startSample{0};
    std::vector<uint32_t> busChannelCounts;
    std::vector<std::vector<float>> channels;
    std::string errorMessage;
};

enum class OfflineTrackRenderStepState {
    InProgress,
    Complete,
    Error,
};

struct OfflineTrackRenderStepResult {
    OfflineTrackRenderStepState state{OfflineTrackRenderStepState::Error};
    OfflineRenderProgress progress;
    std::string errorMessage;
};

OfflineRenderResult renderOfflineProject(SequencerEngine& engine,
                                         const OfflineRenderSettings& settings,
                                         const OfflineRenderCallbacks& callbacks = {});

} // namespace uapmd
