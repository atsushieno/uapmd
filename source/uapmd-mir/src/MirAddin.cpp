#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if UAPMD_ENABLE_LIBSONARE
#include <sonare/sonare_c.h>
#include <umppi/umppi.hpp>
#endif
#include <uapmd-addin-core/uapmd-addin-core.hpp>
#if UAPMD_ENABLE_LIBSONARE
#include <uapmd-engine/uapmd-engine.hpp>
#include "MirDiagnostics.hpp"
#include "MirTempoAnalysis.hpp"
#include "MirRhythmAnalysis.hpp"
#endif

using namespace uapmd_addin;

namespace {

#if UAPMD_ENABLE_LIBSONARE

constexpr std::string_view kCommandExtensionPoint{"/uapmd/app/command/v1"};
constexpr uint32_t kDefaultTickResolution = 480;
constexpr uint8_t kTempoGroup = 0;
constexpr uint8_t kTempoChannel = 0;

enum class MirStage {
    Idle,
    Preparing,
    Analyzing,
    Writing,
};

struct AnalysisResult {
    uapmd::TimelinePosition position;
    uint32_t tick_resolution{kDefaultTickResolution};
    int64_t duration_samples{0};
    double sample_rate{0.0};
    double bpm{120.0};
    uint8_t time_signature_numerator{4};
    uint8_t time_signature_denominator{4};
    std::vector<std::tuple<double, uint8_t, uint8_t>> time_signatures;
    std::vector<std::pair<double, double>> tempo_points;
    std::vector<std::pair<double, std::string>> chords;
};

uint64_t analysisTimeToTicks(const AnalysisResult& result, double seconds);

std::vector<uapmd::MidiTimeSignatureChange> makeTimeSignatureChanges(
        const AnalysisResult& result) {
    const auto signatures = result.time_signatures.empty()
        ? std::vector<std::tuple<double, uint8_t, uint8_t>>{
            {0.0, result.time_signature_numerator, result.time_signature_denominator}}
        : result.time_signatures;
    std::vector<uapmd::MidiTimeSignatureChange> changes;
    changes.reserve(signatures.size());
    for (const auto& [time, numerator, denominator] : signatures)
        changes.push_back({analysisTimeToTicks(result, time), numerator, denominator, 24, 8});
    return changes;
}

std::vector<uapmd::MidiTempoChange> makeTempoChanges(const AnalysisResult& result) {
    std::vector<uapmd::MidiTempoChange> changes;
    changes.reserve(result.tempo_points.size());
    for (const auto& [time, bpm] : result.tempo_points)
        changes.push_back({analysisTimeToTicks(result, time), bpm});
    if (changes.empty())
        changes.push_back({0, result.bpm});
    return changes;
}

double clampBpm(double bpm) {
    return std::clamp(bpm, 0.0001, 960.0);
}

uint32_t bpmToTenNanoseconds(double bpm) {
    return static_cast<uint32_t>(std::clamp(
        6000000000.0 / clampBpm(bpm), 1.0,
        static_cast<double>(std::numeric_limits<uint32_t>::max())));
}

uint64_t analysisTimeToTicks(const AnalysisResult& result, double seconds) {
    return uapmd_mir::tempoMapTimeToTicks(
        result.tempo_points, seconds, result.tick_resolution, result.bpm);
}

std::string chordLabel(const SonareChord& chord) {
    static constexpr std::array<std::string_view, 12> roots{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static constexpr std::array<std::string_view, 17> qualities{
        "", "m", "dim", "aug", "7", "maj7", "m7", "sus2", "sus4", "",
        "add9", "madd9", "dim7", "half-dim7", "maj9", "9", "sus2add4"};

    const auto root = static_cast<size_t>(chord.root);
    const auto quality = static_cast<size_t>(chord.quality);
    if (root >= roots.size())
        return "N.C.";
    if (quality >= qualities.size() || chord.quality == SONARE_CHORD_UNKNOWN)
        return std::string(roots[root]);
    return std::format("{}{}", roots[root], qualities[quality]);
}

void appendUmp(std::vector<uapmd_ump_t>& events, std::vector<uint64_t>& ticks,
               const umppi::Ump& ump, uint64_t tick) {
    const auto count = ump.getSizeInInts();
    const std::array<uint32_t, 4> words{ump.int1, ump.int2, ump.int3, ump.int4};
    for (int index = 0; index < count; ++index) {
        events.push_back(words[static_cast<size_t>(index)]);
        ticks.push_back(tick);
    }
}

void appendDelta(std::vector<uapmd_ump_t>& events, std::vector<uint64_t>& ticks,
                 uint64_t delta, uint64_t absoluteTick) {
    appendUmp(events, ticks,
               umppi::Ump(umppi::UmpFactory::deltaClockstamp(
                   static_cast<uint32_t>(std::min<uint64_t>(delta, 0xFFFFFu)))),
               absoluteTick);
}

std::pair<std::vector<uapmd_ump_t>, std::vector<uint64_t>> makeMasterClip(
        const AnalysisResult& result) {
    std::vector<uapmd_ump_t> events;
    std::vector<uint64_t> ticks;
    appendUmp(events, ticks, umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)), 0);
    appendUmp(events, ticks, umppi::Ump(umppi::UmpFactory::dctpq(
        static_cast<uint16_t>(result.tick_resolution))), 0);
    appendDelta(events, ticks, 0, 0);
    appendUmp(events, ticks, umppi::Ump(umppi::UmpFactory::startOfClip()), 0);

    struct ClipEvent {
        uint64_t tick;
        int priority;
        std::vector<umppi::Ump> messages;
    };
    std::vector<ClipEvent> clipEvents;
    if (result.tempo_points.empty()) {
        clipEvents.push_back({0, 0, {umppi::UmpFactory::tempo(
            kTempoGroup, kTempoChannel, bpmToTenNanoseconds(result.bpm))}});
    } else {
        for (const auto& [time, bpm] : result.tempo_points)
            clipEvents.push_back({analysisTimeToTicks(result, time), 0,
                                  {umppi::UmpFactory::tempo(
                                      kTempoGroup, kTempoChannel, bpmToTenNanoseconds(bpm))}});
    }
    const auto signatures = result.time_signatures.empty()
        ? std::vector<std::tuple<double, uint8_t, uint8_t>>{
            {0.0, result.time_signature_numerator, result.time_signature_denominator}}
        : result.time_signatures;
    for (const auto& [time, numerator, denominator] : signatures) {
        const auto tick = analysisTimeToTicks(result, time);
        clipEvents.push_back({tick, 1, {umppi::UmpFactory::timeSignatureDirect(
            kTempoGroup, kTempoChannel, numerator, denominator, 0)}});
    }

    for (const auto& [time, label] : result.chords) {
        const auto tick = analysisTimeToTicks(result, time);
        clipEvents.push_back({tick, 2, umppi::UmpFactory::metadataText(
            kTempoGroup, 0, kTempoChannel, 0, std::format("MIR chord {}", label))});
    }

    const auto durationSeconds = result.sample_rate > 0.0
        ? static_cast<double>(result.duration_samples) / result.sample_rate : 0.0;
    const auto endTick = analysisTimeToTicks(result, durationSeconds);
    clipEvents.push_back({endTick, 3, {umppi::UmpFactory::endOfClip()}});

    std::ranges::sort(clipEvents, [](const auto& left, const auto& right) {
        if (left.tick != right.tick)
            return left.tick < right.tick;
        return left.priority < right.priority;
    });
    uint64_t lastTick = 0;
    for (const auto& event : clipEvents) {
        appendDelta(events, ticks, event.tick - lastTick, event.tick);
        for (const auto& ump : event.messages)
            appendUmp(events, ticks, ump, event.tick);
        lastTick = event.tick;
    }
    return {std::move(events), std::move(ticks)};
}

bool intervalsOverlap(const uapmd::ProjectClipSnapshot& left,
                      const uapmd::ProjectClipSnapshot& right) {
    const auto leftEnd = left.position.samples + left.durationSamples;
    const auto rightEnd = right.position.samples + right.durationSamples;
    return left.position.samples < rightEnd && right.position.samples < leftEnd;
}

class MirCommand final : public Command {
public:
    explicit MirCommand(uapmd::SequencerEngine& engine)
        : engine_(engine) {}

    ~MirCommand() override {
        stop();
    }

    std::string_view id() const noexcept override {
        return "uapmd-mir.analyze-project";
    }

    std::string_view title() const noexcept override {
        if (!running_.load(std::memory_order_acquire))
            return "Populate master track (libsonare)";

        static thread_local std::string label;
        try {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - startedAt_).count();
            const auto processed = processedSources_.load(std::memory_order_acquire);
            const auto total = totalSources_.load(std::memory_order_acquire);
            const auto stage = stage_.load(std::memory_order_acquire);
            const auto stageName = stage == MirStage::Preparing ? "preparing"
                : stage == MirStage::Analyzing ? "analyzing"
                : stage == MirStage::Writing ? "writing results"
                : "starting";
            if (total > 0)
                label = std::format("Populate master track (libsonare) ({} {}/{}; {}s)",
                                    stageName, processed, total, elapsed);
            else
                label = std::format("Populate master track (libsonare) ({}; {}s)", stageName, elapsed);
        } catch (...) {
            return "Populate master track (libsonare) (running...)";
        }
        return label;
    }

    int order() const noexcept override {
        return 1000;
    }

    bool enabled() const noexcept override {
        return !running_.load(std::memory_order_acquire);
    }

    void invoke() noexcept override {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;

        try {
            startedAt_ = std::chrono::steady_clock::now();
            stage_.store(MirStage::Preparing, std::memory_order_release);
            processedSources_.store(0, std::memory_order_release);
            if (worker_.joinable())
                worker_.join();
            stopRequested_.store(false, std::memory_order_release);
            worker_ = std::thread([this] {
                analyzeProject();
                running_.store(false, std::memory_order_release);
            });
        } catch (...) {
            running_.store(false, std::memory_order_release);
        }
    }

    void stop() noexcept {
        if (worker_.joinable())
            stopRequested_.store(true, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        running_.store(false, std::memory_order_release);
    }

private:
    void analyzeProject() noexcept {
        try {
            uapmd_mir::MirDiagnosticLog diagnostics;
            uapmd_mir::MirDiagnosticLogGuard diagnosticLogGuard{diagnostics};
            auto& timeline = engine_.timeline();
            const auto& view = engine_.timeline().projectDocumentView();
            uint32_t projectTickResolution = timeline.state().projectTickResolution;
            const auto findTickResolution = [&](uapmd::ProjectObjectId trackId) {
                for (const auto& clipId : view.clipIds(trackId)) {
                    const auto clip = view.getClip(clipId);
                    if (clip && clip->clipType == uapmd::ClipType::Midi
                        && !clip->name.starts_with("MIR: ")
                        && clip->tickResolution > 0)
                        return clip->tickResolution;
                }
                return uint32_t{0};
            };
            if (projectTickResolution == 0)
                if (const auto masterTrackId = view.masterTrackId())
                    projectTickResolution = findTickResolution(*masterTrackId);
            if (projectTickResolution == 0) {
                for (const auto& trackId : view.trackIds()) {
                    projectTickResolution = findTickResolution(trackId);
                    if (projectTickResolution > 0)
                        break;
                }
            }
            if (projectTickResolution == 0)
                projectTickResolution = kDefaultTickResolution;

            std::vector<AnalysisResult> results;
            std::vector<uapmd::ProjectClipSnapshot> midiClips;
            for (const auto& trackId : view.trackIds()) {
                for (const auto& clipId : view.clipIds(trackId)) {
                    const auto clip = view.getClip(clipId);
                    if (clip && clip->clipType == uapmd::ClipType::Midi)
                        midiClips.push_back(*clip);
                }
            }

            const auto sourceIds = view.audioSourceIds();
            totalSources_.store(static_cast<int>(sourceIds.size()), std::memory_order_release);
            stage_.store(MirStage::Analyzing, std::memory_order_release);
            for (const auto& sourceId : sourceIds) {
                if (stopRequested_.load(std::memory_order_acquire))
                    return;
                processedSources_.fetch_add(1, std::memory_order_acq_rel);

                const auto source = view.getAudioSource(sourceId);
                if (!source || source->frameCount <= 0 || source->channelCount == 0 || source->sampleRate <= 0)
                    continue;
                const auto sourceClip = view.getClip(source->clipId);
                if (!sourceClip)
                    continue;
                if (std::ranges::any_of(midiClips, [&](const auto& midiClip) {
                        return intervalsOverlap(*sourceClip, midiClip);
                    }))
                    continue;

                const auto frameCount = static_cast<size_t>(source->frameCount);
                std::vector<std::vector<float>> channels(
                    source->channelCount, std::vector<float>(frameCount));
                std::vector<float*> destinations;
                destinations.reserve(channels.size());
                for (auto& channel : channels)
                    destinations.push_back(channel.data());

                if (!view.readAudioSourceSamples(
                        sourceId, 0, source->frameCount, destinations.data(), source->channelCount))
                    continue;

                std::vector<float> mono(frameCount, 0.0f);
                for (size_t frame = 0; frame < frameCount; ++frame) {
                    for (const auto& channel : channels)
                        mono[frame] += channel[frame];
                    mono[frame] /= static_cast<float>(channels.size());
                }

                const auto rawLog = [&diagnostics](std::string_view message) {
                    diagnostics.append(message);
                };
                rawLog(std::format(
                    "libsonare raw source frames={} sample_rate={:.6f} duration={:.6f}s",
                    frameCount, source->sampleRate, frameCount / source->sampleRate));

                AnalysisResult result;
                result.position = sourceClip->position;
                result.tick_resolution = projectTickResolution;
                result.duration_samples = sourceClip->durationSamples;
                result.sample_rate = source->sampleRate;
                result.bpm = 120.0;
                result.tempo_points = uapmd_mir::sonare::detectTempoMap(
                    mono, static_cast<int>(source->sampleRate), result.bpm, rawLog);
                if (!result.tempo_points.empty())
                    result.bpm = result.tempo_points.front().second;

                result.time_signatures = uapmd_mir::sonare::detectRhythmMap(
                    mono, static_cast<int>(source->sampleRate),
                    result.tempo_points, result.bpm, rawLog);
                if (!result.time_signatures.empty()) {
                    result.time_signature_numerator = std::get<1>(result.time_signatures.front());
                    result.time_signature_denominator = std::get<2>(result.time_signatures.front());
                }

                SonareChordDetectionOptions options{};
                options.min_duration = 0.25f;
                options.smoothing_window = 0.5f;
                options.threshold = 0.35f;
                options.use_triads_only = 0;
                options.n_fft = 4096;
                options.hop_length = 512;
                options.use_beat_sync = 1;
                options.use_hmm = 1;
                options.hmm_beam_width = 8;
                options.use_key_context = 0;
                options.detect_inversions = 1;
                options.chroma_method = 1;

                SonareChordAnalysisResult chords{};
                rawLog("uapmd calling libsonare chord analysis");
                const auto chordError = sonare_detect_chords_ex(
                    mono.data(), mono.size(), static_cast<int>(source->sampleRate),
                    &options, &chords);
                if (chordError == SONARE_OK) {
                    rawLog(std::format(
                        "libsonare raw chords count={}", chords.chord_count));
                    for (size_t index = 0; index < chords.chord_count; ++index) {
                        const auto& chord = chords.chords[index];
                        rawLog(std::format(
                            "libsonare raw chord index={} start={:.6f}s end={:.6f}s root={} quality={} bass={} confidence={:.6f}",
                            index, chord.start, chord.end, static_cast<int>(chord.root),
                            static_cast<int>(chord.quality), static_cast<int>(chord.bass),
                            chord.confidence));
                        result.chords.emplace_back(chord.start, chordLabel(chord));
                    }
                    sonare_free_chord_analysis_result(&chords);
                } else
                    rawLog(std::format(
                        "libsonare raw chords error={}", static_cast<int>(chordError)));
                results.push_back(std::move(result));
            }

            std::ranges::sort(results, [](const auto& left, const auto& right) {
                return left.position.samples < right.position.samples;
            });

            auto* master = timeline.masterTimelineTrack();
            if (!master)
                return;
            stage_.store(MirStage::Writing, std::memory_order_release);
            // Populating the master track is purely additive. Whatever the
            // track already holds stays untouched, including the clips of an
            // earlier run of this or the other MIR backend, so that the
            // results of both backends can be compared side by side. The
            // guard below only keeps one run's own results from overlapping
            // each other.
            //
            // Insertion with User origin captures clip fragments for undo
            // history. Those captures must happen outside a document
            // transaction, so let each facade mutation manage its own
            // command step.
            int64_t lastEnd = std::numeric_limits<int64_t>::min();
            for (const auto& result : results) {
                if (result.position.samples < lastEnd)
                    continue;
                auto [events, ticks] = makeMasterClip(result);
                const auto added = timeline.addMasterMidiClip(
                    result.position, std::move(events), std::move(ticks), result.tick_resolution,
                    result.bpm,
                    makeTempoChanges(result),
                    makeTimeSignatureChanges(result),
                    "MIR: libsonare " + std::to_string(result.position.samples), false, "",
                    uapmd::ProjectMutationOrigin::User);
                if (added.success)
                    lastEnd = result.position.samples + result.duration_samples;
            }
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "MIR project analysis failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("MIR project analysis failed");
        }
    }

    uapmd::SequencerEngine& engine_;
    std::atomic<bool> running_{false};
    std::atomic<MirStage> stage_{MirStage::Idle};
    std::atomic<int> processedSources_{0};
    std::atomic<int> totalSources_{0};
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point startedAt_{};
    std::thread worker_;
};

#endif

class MirAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/mir", "analysis"};
    }

    std::string_view name() const noexcept override {
        return "libsonare music information retrieval";
    }

    std::string_view path() const noexcept override {
        return "/uapmd/app/command/v1";
    }

    bool initialize(AddinHost& host) noexcept override {
#if UAPMD_ENABLE_LIBSONARE
        auto* engine = static_cast<uapmd::SequencerEngine*>(
            host.extensionPoint("/uapmd/engine/v1"));
        registry_ = static_cast<CommandRegistry*>(host.extensionPoint(kCommandExtensionPoint));
        if (!engine || !registry_)
            return false;

        try {
            command_ = std::make_unique<MirCommand>(*engine);
            registry_->registerCommand(*command_);
            return true;
        } catch (...) {
            command_.reset();
            registry_ = nullptr;
            return false;
        }
#else
        (void) host;
        return true;
#endif
    }

    void cleanup(AddinHost&) noexcept override {
#if UAPMD_ENABLE_LIBSONARE
        if (registry_ && command_)
            registry_->unregisterCommand(*command_);
        if (command_)
            command_->stop();
        command_.reset();
        registry_ = nullptr;
#endif
    }

private:
#if UAPMD_ENABLE_LIBSONARE
    CommandRegistry* registry_{};
    std::unique_ptr<MirCommand> command_;
#endif
};

MirAddin mirAddin;

} // namespace

// Assembled into the library's AddinEntry by AddinEntry.cpp.
uapmd_addin::Addin* uapmd_mir_analysis_addin() noexcept {
    return &mirAddin;
}
