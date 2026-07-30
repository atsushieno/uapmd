#include <algorithm>
#include <cstring>

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd {

MidiRecorder::MidiRecorder(SequencerEngine& engine) : engine_(engine) {}

bool MidiRecorder::start(Target target) {
    if (target.trackId.empty() || target.clipId < 0)
        return false;
    const auto startSample = engine_.playbackPosition();
    std::lock_guard lock(mutex_);
    target.startSample = startSample;
    target_ = std::move(target);
    start_sample_ = startSample;
        sample_rate_ = std::max(1, engine_.currentSampleRate());
    events_.clear();
    recording_ = true;
    // The recorder is now armed; notify every playback-engine extension after
    // releasing state only in the simple no-op listener case.
    engine_.notifyRecordingStarted();
    return true;
}

void MidiRecorder::playbackStopped() { commit(); }

void MidiRecorder::recordingStopped() { commit(); }

void MidiRecorder::commit() {
    const auto captureTarget = target();
    const auto capturedEvents = takeEvents();
    if (capturedEvents.empty())
        return;
    const auto trackIndex = engine_.timeline().trackIndexForReferenceId(captureTarget.trackId);
    if (trackIndex < 0)
        return;
    auto* track = engine_.timeline().tracks()[static_cast<size_t>(trackIndex)];
    const auto* clip = track ? track->clipManager().getClip(captureTarget.clipId) : nullptr;
    auto source = clip && track ? track->getSourceNode(clip->sourceNodeInstanceId) : nullptr;
    auto midi = std::dynamic_pointer_cast<MidiClipSourceNode>(source);
    if (!midi)
        return;
    const auto ticksPerSecond = std::max(0.0001, midi->clipTempo()) / 60.0 * midi->tickResolution();
    std::vector<uapmd_ump_t> words;
    std::vector<uint64_t> ticks;
    for (const auto& event : capturedEvents) {
        const auto tick = static_cast<uint64_t>(std::llround(
            static_cast<double>(std::max<int64_t>(0,
                captureTarget.startSample + event.samplePosition - clip->position.samples)) *
                ticksPerSecond / sample_rate_));
        words.insert(words.end(), event.words.begin(), event.words.end());
        ticks.insert(ticks.end(), event.words.size(), tick);
    }
    engine_.timeline().appendMidiEventsToClip(
        trackIndex, captureTarget.clipId, std::move(words), std::move(ticks));
}

void MidiRecorder::stop() {
    if (!isRecording())
        return;
    engine_.notifyRecordingStopped();
}

std::vector<MidiRecorder::Event> MidiRecorder::takeEvents() {
    std::lock_guard lock(mutex_);
    recording_ = false;
    auto events = std::move(events_);
    events_.clear();
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.samplePosition < b.samplePosition;
    });
    return events;
}

void MidiRecorder::cancel() {
    std::lock_guard lock(mutex_);
    recording_ = false;
    events_.clear();
}

bool MidiRecorder::isRecording() const {
    std::lock_guard lock(mutex_);
    return recording_;
}

MidiRecorder::Target MidiRecorder::target() const {
    std::lock_guard lock(mutex_);
    return target_;
}

void MidiRecorder::record(std::string_view trackId, const uapmd_ump_t* ump,
                          size_t sizeInBytes, int64_t samplePosition) {
    if (!ump || sizeInBytes == 0 || sizeInBytes % sizeof(uapmd_ump_t) != 0)
        return;
    std::lock_guard lock(mutex_);
    if (!recording_ || target_.trackId != trackId)
        return;
    Event event;
    event.samplePosition = std::max<int64_t>(0, samplePosition - start_sample_);
    event.words.resize(sizeInBytes / sizeof(uapmd_ump_t));
    std::memcpy(event.words.data(), ump, sizeInBytes);
    events_.push_back(std::move(event));
}

} // namespace uapmd
