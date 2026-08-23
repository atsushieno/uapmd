#include "ClipTranscription.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace uapmd_pitch {

namespace {

constexpr uint32_t kDefaultTickResolution = 480;

// Half-width of the resampling kernel, in zero crossings of the sinc. Eight is
// enough for transcription and keeps the filter to a few dozen taps.
constexpr int kResampleLobes = 8;

double sinc(double x) {
    if (std::abs(x) < 1e-9)
        return 1.0;
    const auto pix = std::numbers::pi * x;
    return std::sin(pix) / pix;
}

} // namespace

std::vector<AudioClipSource> collectAudioClipSources(
        const uapmd::ProjectDocumentView& view,
        std::optional<int32_t> only_track_index,
        std::optional<int32_t> only_clip_id) {
    std::vector<AudioClipSource> results;
    for (const auto& sourceId : view.audioSourceIds()) {
        const auto source = view.getAudioSource(sourceId);
        if (!source || source->frameCount <= 0 || source->channelCount == 0
            || source->sampleRate <= 0.0)
            continue;
        const auto clip = view.getClip(source->clipId);
        if (!clip || clip->clipType != uapmd::ClipType::Audio)
            continue;
        if (only_track_index && clip->trackIndex != *only_track_index)
            continue;
        if (only_clip_id && clip->clipNumericId != *only_clip_id)
            continue;
        results.push_back({*clip, *source});
    }
    std::ranges::sort(results, [](const auto& left, const auto& right) {
        if (left.clip.trackIndex != right.clip.trackIndex)
            return left.clip.trackIndex < right.clip.trackIndex;
        return left.clip.position.samples < right.clip.position.samples;
    });
    return results;
}

bool readMono(const uapmd::ProjectDocumentView& view,
              const uapmd::ProjectAudioSourceSnapshot& source,
              std::vector<float>& mono) {
    const auto frameCount = static_cast<size_t>(source.frameCount);
    std::vector<std::vector<float>> channels(
        source.channelCount, std::vector<float>(frameCount));
    std::vector<float*> destinations;
    destinations.reserve(channels.size());
    for (auto& channel : channels)
        destinations.push_back(channel.data());

    if (!view.readAudioSourceSamples(
            source.audioSourceId, 0, source.frameCount,
            destinations.data(), source.channelCount))
        return false;

    mono.assign(frameCount, 0.0f);
    for (size_t frame = 0; frame < frameCount; ++frame) {
        for (const auto& channel : channels)
            mono[frame] += channel[frame];
        mono[frame] /= static_cast<float>(channels.size());
    }
    return true;
}

std::vector<float> resample(const std::vector<float>& input,
                            double source_rate,
                            double target_rate) {
    if (input.empty() || source_rate <= 0.0 || target_rate <= 0.0
        || std::abs(source_rate - target_rate) < 1e-9)
        return input;

    const auto ratio = target_rate / source_rate;
    // Cutoff relative to the source Nyquist: when downsampling it is the target
    // Nyquist that limits, and when upsampling nothing new can be added.
    const auto cutoff = std::min(1.0, ratio);
    const auto halfWidth = static_cast<int>(std::ceil(kResampleLobes / cutoff));

    const auto outputLength = std::max<size_t>(
        1, static_cast<size_t>(std::llround(static_cast<double>(input.size()) * ratio)));
    std::vector<float> output(outputLength, 0.0f);
    const auto length = static_cast<int64_t>(input.size());

    for (size_t i = 0; i < outputLength; ++i) {
        const auto centre = static_cast<double>(i) / ratio;
        const auto first = static_cast<int64_t>(std::floor(centre)) - halfWidth + 1;
        const auto last = static_cast<int64_t>(std::floor(centre)) + halfWidth;
        double sum = 0.0;
        double weightSum = 0.0;
        for (auto k = first; k <= last; ++k) {
            if (k < 0 || k >= length)
                continue;
            const auto offset = static_cast<double>(k) - centre;
            // Blackman window over the kernel's support, which keeps the
            // stopband down far enough that aliasing stays inaudible.
            const auto phase = (offset + halfWidth) / (2.0 * halfWidth);
            const auto window = 0.42
                - 0.5 * std::cos(2.0 * std::numbers::pi * phase)
                + 0.08 * std::cos(4.0 * std::numbers::pi * phase);
            const auto weight = cutoff * sinc(cutoff * offset) * window;
            sum += static_cast<double>(input[static_cast<size_t>(k)]) * weight;
            weightSum += weight;
        }
        // Normalising by the realised weight keeps the gain flat where the
        // kernel is truncated by the ends of the signal.
        output[i] = static_cast<float>(weightSum > 1e-9 ? sum / weightSum : sum);
    }
    return output;
}

bool writeNoteClip(uapmd::SequencerEngine& engine,
                   const AudioClipSource& audio,
                   const std::vector<TranscribedNote>& notes,
                   std::string_view name_prefix) {
    if (notes.empty())
        return false;

    auto& timeline = engine.timeline();
    const auto& state = timeline.state();
    const auto bpm = state.tempo > 0.0 ? state.tempo : 120.0;
    const auto tickResolution = state.projectTickResolution > 0
        ? state.projectTickResolution : kDefaultTickResolution;

    auto [events, ticks] = makeNoteClip(notes, tickResolution, bpm);
    const auto result = timeline.addMidiClipToTrack(
        audio.clip.trackIndex, audio.clip.position,
        std::move(events), std::move(ticks),
        tickResolution, bpm, {}, {},
        audio.clip.name.empty()
            ? std::string{name_prefix}
            : std::format("{}: {}", name_prefix, audio.clip.name),
        false, false,
        uapmd::ProjectMutationOrigin::User);
    if (!result.success)
        return false;

    // A MIDI clip is otherwise only as long as its last note, which leaves
    // nowhere to draw automation over the tail of the audio it came from.
    if (audio.clip.durationSamples > 0)
        engine.commands().resizeClip(
            audio.clip.trackIndex, result.clipId, audio.clip.durationSamples,
            uapmd::ProjectMutationOrigin::Internal);
    return true;
}

} // namespace uapmd_pitch
