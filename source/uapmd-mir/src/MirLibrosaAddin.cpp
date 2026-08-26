#include <algorithm>
#include <array>
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
#include <vector>

#include <umppi/umppi.hpp>
#include <uapmd-addin-core/uapmd-addin-core.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

#include "MainThreadTimelineEdit.hpp"
#include "MirDiagnostics.hpp"
#include "MirLibrosaAnalysis.hpp"

using namespace uapmd_addin;

namespace {

constexpr std::string_view kCommandExtensionPoint{"/uapmd/app/command/v1"};
constexpr uint32_t kDefaultTickResolution = 480;
constexpr uint8_t kTempoGroup = 0;
constexpr uint8_t kTempoChannel = 0;

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

uint64_t analysisTimeToTicks(const AnalysisResult& result, double seconds) {
    return uapmd_mir::tempoMapTimeToTicks(
        result.tempo_points, seconds, result.tick_resolution, result.bpm);
}

double clampBpm(double bpm) {
    return std::clamp(bpm, 0.0001, 960.0);
}

uint32_t bpmToTenNanoseconds(double bpm) {
    return static_cast<uint32_t>(std::clamp(
        6000000000.0 / clampBpm(bpm), 1.0,
        static_cast<double>(std::numeric_limits<uint32_t>::max())));
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

class LibrosaCommand final : public Command {
public:
    explicit LibrosaCommand(uapmd::SequencerEngine& engine) : engine_(engine) {}
    ~LibrosaCommand() override { stop(); }

    std::string_view id() const noexcept override {
        return "uapmd-mir.populate-master-librosa";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{"Populate master track (librosa.cpp)"};
        if (!running_.load(std::memory_order_acquire))
            return base;
        // While a run is in flight the command cancels it, so the label says
        // so -- still carrying the elapsed time.
        static thread_local std::string label;
        try {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - startedAt_).count();
            if (cancelling())
                label = std::format("Cancelling {} ({}s)", base, elapsed);
            else
                label = std::format("Cancel {} (analyzing {}/{}; {}s)", base,
                                    processedSources_.load(std::memory_order_acquire),
                                    totalSources_.load(std::memory_order_acquire), elapsed);
        } catch (...) {
            return "Cancel Populate master track (librosa.cpp)";
        }
        return label;
    }

    int order() const noexcept override { return 1001; }
    // True once cancellation was asked for but the worker has not wound down.
    bool cancelling() const noexcept {
        return running_.load(std::memory_order_acquire)
            && stopRequested_.load(std::memory_order_acquire);
    }

    // Asks the worker to stop without waiting for it. Cancelling runs on the UI
    // thread and the worker only notices between audio sources, so joining here
    // would freeze the interface for as long as one source takes to analyze.
    void requestStop() noexcept {
        stopRequested_.store(true, std::memory_order_release);
    }

    bool enabled() const noexcept override {
        // Never greyed out: while running, activating it cancels.
        return true;
    }

    void invoke() noexcept override {
        if (running_.load(std::memory_order_acquire)) {
            requestStop();
            return;
        }
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        try {
            startedAt_ = std::chrono::steady_clock::now();
            processedSources_.store(0, std::memory_order_release);
            if (worker_.joinable())
                worker_.join();
            stopRequested_.store(false, std::memory_order_release);
            worker_ = std::thread([this] {
                // A run that got as far as queueing its results stays
                // "running" until the main thread has applied them.
                if (!analyzeProject())
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
        // Only once the worker is gone, so nothing is queuing edits any more.
        // This is the teardown path, so an edit still waiting for the event
        // loop is dropped rather than run against an engine on its way out.
        edit_lifetime_.reset();
        running_.store(false, std::memory_order_release);
    }

private:
    // Returns true when the results were handed to the main thread, which is
    // then what finishes the run.
    bool analyzeProject() noexcept {
        try {
            uapmd_mir::MirDiagnosticLog diagnostics;
            uapmd_mir::MirDiagnosticLogGuard diagnosticLogGuard{diagnostics};
            const uapmd_mir::AnalysisLogger rawLog = [&diagnostics](std::string_view message) {
                diagnostics.append(message);
            };

            auto& timeline = engine_.timeline();
            const auto& view = timeline.projectDocumentView();
            auto tickResolution = timeline.state().projectTickResolution;
            if (tickResolution == 0)
                tickResolution = kDefaultTickResolution;

            std::vector<uapmd::ProjectClipSnapshot> midiClips;
            for (const auto& trackId : view.trackIds())
                for (const auto& clipId : view.clipIds(trackId))
                    if (const auto clip = view.getClip(clipId);
                        clip && clip->clipType == uapmd::ClipType::Midi)
                        midiClips.push_back(*clip);

            const auto sourceIds = view.audioSourceIds();
            totalSources_.store(static_cast<int>(sourceIds.size()), std::memory_order_release);
            std::vector<AnalysisResult> results;
            for (const auto sourceId : sourceIds) {
                if (stopRequested_.load(std::memory_order_acquire))
                    return false;
                processedSources_.fetch_add(1, std::memory_order_acq_rel);
                const auto source = view.getAudioSource(sourceId);
                if (!source || source->frameCount <= 0 || source->channelCount == 0
                    || source->sampleRate <= 0)
                    continue;
                const auto sourceClip = view.getClip(source->clipId);
                if (!sourceClip || std::ranges::any_of(midiClips, [&](const auto& midiClip) {
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
                if (!view.readAudioSourceSamples(sourceId, 0, source->frameCount,
                                                 destinations.data(), source->channelCount))
                    continue;

                std::vector<float> mono(frameCount, 0.0f);
                for (size_t frame = 0; frame < frameCount; ++frame) {
                    for (const auto& channel : channels)
                        mono[frame] += channel[frame];
                    mono[frame] /= static_cast<float>(channels.size());
                }

                rawLog(std::format(
                    "librosa.cpp raw source frames={} sample_rate={:.6f} duration={:.6f}s",
                    frameCount, source->sampleRate, frameCount / source->sampleRate));

                AnalysisResult result;
                result.position = sourceClip->position;
                result.tick_resolution = tickResolution;
                result.duration_samples = sourceClip->durationSamples;
                result.sample_rate = source->sampleRate;
                result.bpm = 120.0;
                result.tempo_points = uapmd_mir::librosa_cpp::detectTempoMap(
                    mono, static_cast<int>(source->sampleRate), result.bpm, rawLog);
                if (!result.tempo_points.empty())
                    result.bpm = result.tempo_points.front().second;

                result.time_signatures = uapmd_mir::librosa_cpp::detectRhythmMap(
                    mono, static_cast<int>(source->sampleRate),
                    result.tempo_points, result.bpm, rawLog);
                if (!result.time_signatures.empty()) {
                    result.time_signature_numerator = std::get<1>(result.time_signatures.front());
                    result.time_signature_denominator = std::get<2>(result.time_signatures.front());
                }

                result.chords = uapmd_mir::librosa_cpp::detectChords(
                    mono, static_cast<int>(source->sampleRate), rawLog);
                results.push_back(std::move(result));
            }

            std::ranges::sort(results, [](const auto& left, const auto& right) {
                return left.position.samples < right.position.samples;
            });
            // The analysis is over; all that is left mutates the timeline,
            // which is main-thread work -- see MainThreadTimelineEdit.hpp.
            uapmd_mir::runTimelineEditOnMainThread(
                edit_lifetime_,
                [this, results = std::move(results)] {
                    // Cancelling stays meaningful right up to the moment the
                    // edit lands: a run that was cancelled leaves the project
                    // alone, the same as one cancelled mid-analysis.
                    if (!stopRequested_.load(std::memory_order_acquire))
                        writeResults(results);
                    running_.store(false, std::memory_order_release);
                });
            return true;
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "librosa.cpp MIR project analysis failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("librosa.cpp MIR project analysis failed");
        }
        return false;
    }

    // Runs on the main thread.
    void writeResults(const std::vector<AnalysisResult>& results) noexcept {
        try {
            auto& timeline = engine_.timeline();
            if (!timeline.masterTimelineTrack())
                return;
            // Populating the master track is purely additive; see the comment
            // in MirAddin.cpp for why nothing existing is removed here.
            int64_t lastEnd = std::numeric_limits<int64_t>::min();
            for (const auto& result : results) {
                if (result.position.samples < lastEnd)
                    continue;
                auto [events, ticks] = makeMasterClip(result);
                const auto added = timeline.addMasterMidiClip(
                    result.position, std::move(events), std::move(ticks), result.tick_resolution,
                    result.bpm, makeTempoChanges(result), makeTimeSignatureChanges(result),
                    "MIR: librosa.cpp " + std::to_string(result.position.samples), false, "",
                    uapmd::ProjectMutationOrigin::User);
                if (added.success)
                    lastEnd = result.position.samples + result.duration_samples;
            }
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "librosa.cpp MIR result write failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("librosa.cpp MIR result write failed");
        }
    }

    uapmd::SequencerEngine& engine_;
    std::atomic<bool> running_{false};
    std::atomic<int> processedSources_{0};
    std::atomic<int> totalSources_{0};
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point startedAt_{};
    // Ties the queued result write to this command's own lifetime.
    uapmd_mir::AsyncEditLifetimeRef edit_lifetime_{uapmd_mir::makeAsyncEditLifetime()};
    std::thread worker_;
};

class LibrosaAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override { return {"/uapmd/mir", "librosa"}; }
    std::string_view name() const noexcept override {
        return "librosa.cpp music information retrieval";
    }
    std::string_view path() const noexcept override { return kCommandExtensionPoint; }

    bool initialize(AddinHost& host) noexcept override {
        auto* engine = static_cast<uapmd::SequencerEngine*>(
            host.extensionPoint("/uapmd/engine/v1"));
        registry_ = static_cast<CommandRegistry*>(host.extensionPoint(kCommandExtensionPoint));
        if (!engine || !registry_)
            return false;
        try {
            command_ = std::make_unique<LibrosaCommand>(*engine);
            registry_->registerCommand(*command_);
            return true;
        } catch (...) {
            command_.reset();
            registry_ = nullptr;
            return false;
        }
    }

    void cleanup(AddinHost&) noexcept override {
        if (registry_ && command_)
            registry_->unregisterCommand(*command_);
        if (command_)
            command_->stop();
        command_.reset();
        registry_ = nullptr;
    }

private:
    CommandRegistry* registry_{};
    std::unique_ptr<LibrosaCommand> command_;
};

} // namespace

uapmd_addin::Addin* uapmd_mir_librosa_addin() noexcept {
    static LibrosaAddin addin;
    return &addin;
}
