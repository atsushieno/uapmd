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

#include <librosa/beat.hpp>
#include <librosa/feature/spectral.hpp>
#include <librosa/onset.hpp>
#include <umppi/umppi.hpp>
#include <uapmd-addin-core/uapmd-addin-core.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

using namespace uapmd_addin;

namespace {

constexpr std::string_view kCommandExtensionPoint{"/uapmd/app/command/v1"};
constexpr uint32_t kDefaultTickResolution = 480;
constexpr int kHopLength = 512;
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

uint64_t timeToTicks(const AnalysisResult& result, double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0 || result.tick_resolution == 0)
        return 0;
    const auto bpm = std::clamp(result.bpm, 0.0001, 960.0);
    return static_cast<uint64_t>(std::llround(
        seconds * bpm / 60.0 * result.tick_resolution));
}

uint32_t bpmToTenNanoseconds(double bpm) {
    return static_cast<uint32_t>(std::clamp(
        6000000000.0 / std::clamp(bpm, 0.0001, 960.0), 1.0,
        static_cast<double>(std::numeric_limits<uint32_t>::max())));
}

std::vector<uapmd::MidiTempoChange> makeTempoChanges(const AnalysisResult& result) {
    std::vector<uapmd::MidiTempoChange> changes;
    for (const auto& [time, bpm] : result.tempo_points)
        changes.push_back({timeToTicks(result, time), bpm});
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
    for (const auto& [time, numerator, denominator] : signatures)
        changes.push_back({timeToTicks(result, time), numerator, denominator, 24, 8});
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
    for (const auto& [time, bpm] : result.tempo_points)
        clipEvents.push_back({timeToTicks(result, time), 0,
                              {umppi::UmpFactory::tempo(
                                  kTempoGroup, kTempoChannel, bpmToTenNanoseconds(bpm))}});
    if (clipEvents.empty())
        clipEvents.push_back({0, 0, {umppi::UmpFactory::tempo(
            kTempoGroup, kTempoChannel, bpmToTenNanoseconds(result.bpm))}});

    const auto signatures = result.time_signatures.empty()
        ? std::vector<std::tuple<double, uint8_t, uint8_t>>{
            {0.0, result.time_signature_numerator, result.time_signature_denominator}}
        : result.time_signatures;
    for (const auto& [time, numerator, denominator] : signatures)
        clipEvents.push_back({timeToTicks(result, time), 1,
                              {umppi::UmpFactory::timeSignatureDirect(
                                  kTempoGroup, kTempoChannel, numerator, denominator, 0)}});
    for (const auto& [time, label] : result.chords)
        clipEvents.push_back({timeToTicks(result, time), 2,
                              umppi::UmpFactory::metadataText(
                                  kTempoGroup, 0, kTempoChannel, 0,
                                  std::format("MIR chord {}", label))});

    const auto duration = result.sample_rate > 0.0
        ? static_cast<double>(result.duration_samples) / result.sample_rate : 0.0;
    const auto endTick = timeToTicks(result, duration);
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

struct MeterCandidate {
    int numerator;
    int denominator;
    std::vector<int> grouping;
};

std::vector<MeterCandidate> meterCandidates() {
    static constexpr std::array<int, 10> numerators{3, 4, 5, 6, 7, 8, 9, 11, 12, 13};
    static const std::array<std::vector<std::vector<int>>, 14> groupings{
        std::vector<std::vector<int>>{},
        std::vector<std::vector<int>>{},
        std::vector<std::vector<int>>{{2}},
        std::vector<std::vector<int>>{{3}},
        std::vector<std::vector<int>>{{4}, {2, 2}},
        std::vector<std::vector<int>>{{2, 3}, {3, 2}},
        std::vector<std::vector<int>>{{3, 3}, {2, 2, 2}},
        std::vector<std::vector<int>>{{3, 2, 2}, {2, 3, 2}, {2, 2, 3}},
        std::vector<std::vector<int>>{{4, 4}, {3, 3, 2}, {3, 2, 3}, {2, 3, 3}},
        std::vector<std::vector<int>>{{3, 3, 3}, {2, 2, 2, 3}, {2, 2, 3, 2},
                                      {2, 3, 2, 2}, {3, 2, 2, 2}},
        std::vector<std::vector<int>>{{3, 3, 2, 2}, {3, 2, 3, 2}, {2, 3, 3, 2},
                                      {2, 2, 3, 3}},
        std::vector<std::vector<int>>{{3, 3, 3, 2}, {3, 3, 2, 3}, {3, 2, 3, 3},
                                      {2, 3, 3, 3}},
        std::vector<std::vector<int>>{{3, 3, 3, 3}, {4, 4, 4}},
        std::vector<std::vector<int>>{{3, 3, 3, 2, 2}, {3, 3, 2, 3, 2},
                                      {3, 2, 3, 3, 2}, {2, 3, 3, 3, 2}},
    };
    std::vector<MeterCandidate> result;
    for (const auto numerator : numerators) {
        const auto denominator = numerator >= 5 && numerator != 12 ? 8 : 4;
        for (const auto& grouping : groupings[static_cast<size_t>(numerator)])
            result.push_back({numerator, denominator, grouping});
    }
    return result;
}

double meterStrength(const librosa::ArrayXr& onset,
                     const std::vector<Eigen::Index>& beats, size_t index) {
    const auto frame = beats[index];
    if (frame < 0 || frame >= onset.size())
        return 0.0;
    return onset(frame);
}

double scoreMeterWindow(const librosa::ArrayXr& onset,
                        const std::vector<Eigen::Index>& beats,
                        size_t start, size_t end, const MeterCandidate& candidate) {
    std::vector<int> groupStarts{0};
    int cursor = 0;
    for (size_t index = 0; index + 1 < candidate.grouping.size(); ++index) {
        cursor += candidate.grouping[index];
        groupStarts.push_back(cursor);
    }
    double downbeat = 0.0;
    double secondary = 0.0;
    double weak = 0.0;
    size_t downbeatCount = 0;
    size_t secondaryCount = 0;
    size_t weakCount = 0;
    for (size_t index = start; index < end; ++index) {
        const auto position = static_cast<int>((index - start)
            % static_cast<size_t>(candidate.numerator));
        const auto strength = meterStrength(onset, beats, index);
        if (position == 0) {
            downbeat += strength;
            ++downbeatCount;
        } else if (std::ranges::find(groupStarts, position) != groupStarts.end()) {
            secondary += strength;
            ++secondaryCount;
        } else {
            weak += strength;
            ++weakCount;
        }
    }
    if (downbeatCount == 0 || weakCount == 0)
        return -std::numeric_limits<double>::infinity();
    const auto weakMean = weak / weakCount;
    auto score = downbeat / downbeatCount - weakMean;
    if (secondaryCount > 0)
        score += 0.55 * (secondary / secondaryCount - weakMean);

    size_t completeBars = 0;
    double patternError = 0.0;
    std::vector<double> pattern(static_cast<size_t>(candidate.numerator));
    for (size_t index = start; index + static_cast<size_t>(candidate.numerator) <= end;
         index += static_cast<size_t>(candidate.numerator)) {
        for (int position = 0; position < candidate.numerator; ++position)
            pattern[static_cast<size_t>(position)] += meterStrength(
                onset, beats, index + static_cast<size_t>(position));
        ++completeBars;
    }
    if (completeBars >= 2) {
        for (auto& value : pattern)
            value /= completeBars;
        for (size_t index = start;
             index + static_cast<size_t>(candidate.numerator) <= end;
             index += static_cast<size_t>(candidate.numerator))
            for (int position = 0; position < candidate.numerator; ++position)
                patternError += std::abs(meterStrength(
                    onset, beats, index + static_cast<size_t>(position))
                    - pattern[static_cast<size_t>(position)]);
        score += 0.35 * std::max(0.0, 1.0 - patternError
            / (completeBars * candidate.numerator));
    }
    return score;
}

std::vector<std::tuple<double, uint8_t, uint8_t>> estimateMeter(
        const librosa::ArrayXr& onset, const std::vector<Eigen::Index>& beats,
        double sampleRate) {
    if (beats.size() < 8)
        return {};
    const auto candidates = meterCandidates();
    constexpr size_t maxSegmentBeats = 96;
    constexpr double transitionPenalty = 1.15;
    const auto count = beats.size();
    std::vector<double> dp(count + 1, -std::numeric_limits<double>::infinity());
    std::vector<int> previous(count + 1, -1);
    std::vector<int> selected(count + 1, -1);
    dp[0] = 0.0;
    for (size_t end = 1; end <= count; ++end) {
        const auto first = end > maxSegmentBeats ? end - maxSegmentBeats : 0;
        for (size_t start = first; start < end; ++start) {
            if (!std::isfinite(dp[start]))
                continue;
            for (size_t candidateIndex = 0; candidateIndex < candidates.size();
                 ++candidateIndex) {
                const auto& candidate = candidates[candidateIndex];
                const auto length = end - start;
                if (length < static_cast<size_t>(candidate.numerator * 2)
                    || length % static_cast<size_t>(candidate.numerator) != 0)
                    continue;
                const auto score = scoreMeterWindow(
                    onset, beats, start, end, candidate);
                if (!std::isfinite(score))
                    continue;
                const auto objective = dp[start] + score * length
                    - (start == 0 ? 0.0 : transitionPenalty);
                if (objective > dp[end]) {
                    dp[end] = objective;
                    previous[end] = static_cast<int>(start);
                    selected[end] = static_cast<int>(candidateIndex);
                }
            }
        }
    }
    if (previous[count] < 0)
        return {};

    struct MeterSegment {
        size_t start;
        MeterCandidate candidate;
    };
    std::vector<MeterSegment> reversed;
    for (auto end = count; end > 0;) {
        const auto start = static_cast<size_t>(previous[end]);
        reversed.push_back({start, candidates[static_cast<size_t>(selected[end])]});
        end = start;
    }
    std::ranges::reverse(reversed);
    std::vector<MeterSegment> merged;
    for (const auto& segment : reversed) {
        if (!merged.empty()
            && merged.back().candidate.numerator == segment.candidate.numerator
            && merged.back().candidate.denominator == segment.candidate.denominator
            && merged.back().candidate.grouping == segment.candidate.grouping)
            continue;
        merged.push_back(segment);
    }
    std::vector<std::tuple<double, uint8_t, uint8_t>> result;
    result.reserve(merged.size());
    for (const auto& segment : merged) {
        const auto frame = beats[segment.start];
        result.emplace_back(segment.start == 0 ? 0.0
                            : static_cast<double>(std::max<Eigen::Index>(0, frame))
                                * kHopLength / sampleRate,
                            static_cast<uint8_t>(segment.candidate.numerator),
                            static_cast<uint8_t>(segment.candidate.denominator));
    }
    return result;
}

void restoreLeadingBeatGrid(std::vector<Eigen::Index>& beats, double bpm,
                            double sampleRate) {
    if (beats.empty() || !std::isfinite(bpm) || bpm <= 0.0 || sampleRate <= 0.0)
        return;
    const auto framesPerBeat = sampleRate * 60.0 / bpm / kHopLength;
    if (!std::isfinite(framesPerBeat) || framesPerBeat < 1.0)
        return;
    const auto first = static_cast<double>(beats.front());
    std::vector<Eigen::Index> leading;
    auto frame = first;
    while (frame - framesPerBeat >= -0.5) {
        frame -= framesPerBeat;
        leading.push_back(static_cast<Eigen::Index>(std::llround(frame)));
    }
    if (leading.empty())
        return;
    std::ranges::reverse(leading);
    beats.insert(beats.begin(), leading.begin(), leading.end());
}

std::vector<std::pair<double, std::string>> detectChords(
        const librosa::ArrayXXr& chroma, double sampleRate) {
    static constexpr std::array<std::string_view, 12> roots{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::vector<std::pair<double, std::string>> result;
    std::string previous;
    for (Eigen::Index frame = 0; frame < chroma.cols(); ++frame) {
        auto bestScore = -std::numeric_limits<double>::infinity();
        int bestRoot = 0;
        bool minor = false;
        for (int root = 0; root < 12; ++root) {
            double majorScore = 0.0;
            double minorScore = 0.0;
            for (int interval = 0; interval < 12; ++interval) {
                const auto value = chroma((root + interval) % 12, frame);
                majorScore += value * (interval == 0 || interval == 7 ? 1.0
                    : interval == 4 ? 0.8 : interval == 3 || interval == 8 ? 0.2 : -0.2);
                minorScore += value * (interval == 0 || interval == 7 ? 1.0
                    : interval == 3 ? 0.8 : interval == 4 || interval == 8 ? 0.2 : -0.2);
            }
            if (majorScore > bestScore) {
                bestScore = majorScore;
                bestRoot = root;
                minor = false;
            }
            if (minorScore > bestScore) {
                bestScore = minorScore;
                bestRoot = root;
                minor = true;
            }
        }
        const auto label = std::format("{}{}", roots[bestRoot], minor ? "m" : "");
        if (label != previous) {
            result.emplace_back(static_cast<double>(frame * kHopLength) / sampleRate, label);
            previous = label;
        }
    }
    return result;
}

class LibrosaCommand final : public Command {
public:
    explicit LibrosaCommand(uapmd::SequencerEngine& engine) : engine_(engine) {}
    ~LibrosaCommand() override { stop(); }

    std::string_view id() const noexcept override {
        return "uapmd-mir.populate-master-librosa";
    }

    std::string_view title() const noexcept override {
        if (!running_.load(std::memory_order_acquire))
            return "Populate master track (librosa.cpp)";
        static thread_local std::string label;
        try {
            label = std::format("Populate master track (librosa.cpp) (analyzing {}/{}; {}s)",
                                processedSources_.load(std::memory_order_acquire),
                                totalSources_.load(std::memory_order_acquire),
                                std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - startedAt_).count());
        } catch (...) {
            return "Populate master track (librosa.cpp) (running...)";
        }
        return label;
    }

    int order() const noexcept override { return 1001; }
    bool enabled() const noexcept override {
        return !running_.load(std::memory_order_acquire);
    }

    void invoke() noexcept override {
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
                    return;
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
                for (auto& channel : channels)
                    destinations.push_back(channel.data());
                if (!view.readAudioSourceSamples(sourceId, 0, source->frameCount,
                                                 destinations.data(), source->channelCount))
                    continue;
                librosa::ArrayXr mono(static_cast<Eigen::Index>(frameCount));
                for (size_t frame = 0; frame < frameCount; ++frame) {
                    mono(static_cast<Eigen::Index>(frame)) = 0.0;
                    for (const auto& channel : channels)
                        mono(static_cast<Eigen::Index>(frame)) += channel[frame];
                    mono(static_cast<Eigen::Index>(frame)) /= channels.size();
                }

                const auto sampleRate = static_cast<double>(source->sampleRate);
                const auto onset = librosa::onset::onset_strength(
                    mono, sampleRate, 2048, kHopLength);
                const auto [estimatedTempo, beats] = librosa::beat::beat_track(
                    onset, sampleRate, kHopLength, 120.0, 100.0, false);
                auto beatGrid = beats;
                restoreLeadingBeatGrid(beatGrid, estimatedTempo, sampleRate);
                AnalysisResult result;
                result.position = sourceClip->position;
                result.tick_resolution = tickResolution;
                result.duration_samples = sourceClip->durationSamples;
                result.sample_rate = sampleRate;
                result.bpm = std::isfinite(estimatedTempo) && estimatedTempo > 0.0
                    ? estimatedTempo : 120.0;
                result.tempo_points = {{0.0, result.bpm}};
                result.time_signatures = estimateMeter(onset, beatGrid, sampleRate);
                if (!result.time_signatures.empty()) {
                    result.time_signature_numerator = std::get<1>(result.time_signatures.front());
                    result.time_signature_denominator = std::get<2>(result.time_signatures.front());
                }
                result.chords = detectChords(librosa::feature::chroma_stft(
                    mono, sampleRate, 4096, kHopLength), sampleRate);
                results.push_back(std::move(result));
            }

            std::ranges::sort(results, [](const auto& left, const auto& right) {
                return left.position.samples < right.position.samples;
            });
            auto* master = timeline.masterTimelineTrack();
            if (!master)
                return;
            for (const auto& clip : master->clipManager().getAllClips())
                if (clip.name.starts_with("MIR: "))
                    timeline.removeClipFromTrack(uapmd::kMasterTrackIndex, clip.clipId,
                                                 uapmd::ProjectMutationOrigin::User);
            int64_t lastEnd = std::numeric_limits<int64_t>::min();
            for (const auto& result : results) {
                if (result.position.samples < lastEnd)
                    continue;
                auto [events, ticks] = makeMasterClip(result);
                const auto added = timeline.addMasterMidiClip(
                    result.position, std::move(events), std::move(ticks), result.tick_resolution,
                    result.bpm, makeTempoChanges(result), makeTimeSignatureChanges(result),
                    "MIR: " + std::to_string(result.position.samples), false, "",
                    uapmd::ProjectMutationOrigin::User);
                if (added.success)
                    lastEnd = result.position.samples + result.duration_samples;
            }
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "librosa.cpp MIR project analysis failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("librosa.cpp MIR project analysis failed");
        }
    }

    uapmd::SequencerEngine& engine_;
    std::atomic<bool> running_{false};
    std::atomic<int> processedSources_{0};
    std::atomic<int> totalSources_{0};
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point startedAt_{};
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
