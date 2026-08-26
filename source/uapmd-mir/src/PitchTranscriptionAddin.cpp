#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <uapmd-addin-core/uapmd-addin-core.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

#include "ClipTranscription.hpp"
#include "MainThreadTimelineEdit.hpp"
#include "NoteClipWriter.hpp"
#include "PitchTranscription.hpp"

using namespace uapmd_addin;

namespace {

constexpr std::string_view kCommandExtensionPoint{"/uapmd/app/command/v1"};
constexpr std::string_view kClipCommandExtensionPoint{"/uapmd/app/clip-command/v1"};
constexpr std::string_view kEngineExtensionPoint{"/uapmd/engine/v1"};
// Everything both commands do, minus how the clips to work on were chosen.
// Runs on a worker thread; `stopRequested` lets a shutdown cut it short.
class Transcriber {
public:
    explicit Transcriber(uapmd::SequencerEngine& engine) : engine_(engine) {}

    // Drops whatever clip insertions are still queued for the main thread.
    // Called once the worker is joined, so nothing lands after teardown.
    void revokeQueuedEdits() noexcept { edit_lifetime_.reset(); }

    void run(std::optional<int32_t> trackIndex,
             std::optional<int32_t> clipId,
             const std::atomic<bool>& stopRequested,
             std::atomic<int>& processed,
             std::atomic<int>& total) noexcept {
        try {
            const auto& view = engine_.timeline().projectDocumentView();
            const auto sources = uapmd_pitch::collectAudioClipSources(view, trackIndex, clipId);
            total.store(static_cast<int>(sources.size()), std::memory_order_release);

            for (const auto& [clip, source] : sources) {
                if (stopRequested.load(std::memory_order_acquire))
                    break;
                processed.fetch_add(1, std::memory_order_acq_rel);

                std::vector<float> mono;
                if (!uapmd_pitch::readMono(view, source, mono))
                    continue;

                uapmd_pitch::PitchTranscriptionOptions options;
                const auto notes = uapmd_pitch::transcribeMonoToNotes(
                    mono, source.sampleRate, options,
                    [&stopRequested](double) {
                        return !stopRequested.load(std::memory_order_acquire);
                    });
                // A cancelled run returns whatever it had segmented so far.
                // Writing that would leave a clip covering only part of the
                // audio, which is worse than leaving none.
                if (stopRequested.load(std::memory_order_acquire))
                    break;
                uapmd_pitch::writeNoteClip(
                    engine_, edit_lifetime_, {clip, source}, notes, "Transcribed");
            }
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "MIDI transcription failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("MIDI transcription failed");
        }
    }

private:
    uapmd::SequencerEngine& engine_;
    // Ties every queued clip insertion to this transcriber's own lifetime.
    uapmd_mir::AsyncEditLifetimeRef edit_lifetime_{uapmd_mir::makeAsyncEditLifetime()};
};

// One worker thread shared by both commands, so that the per-clip and
// whole-project entry points cannot run over each other.
class TranscriptionJob {
public:
    explicit TranscriptionJob(uapmd::SequencerEngine& engine) : transcriber_(engine) {}

    ~TranscriptionJob() {
        stop();
    }

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // True once cancellation was asked for but the worker has not wound down.
    bool cancelling() const noexcept {
        return running() && stop_requested_.load(std::memory_order_acquire);
    }

    // Asks the worker to stop without waiting for it. Cancelling runs on the UI
    // thread, and the worker only notices between units of work -- a whole
    // analysis window for Basic Pitch -- so joining here would visibly stall.
    void requestStop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

    int processed() const noexcept { return processed_.load(std::memory_order_acquire); }
    int total() const noexcept { return total_.load(std::memory_order_acquire); }

    std::chrono::steady_clock::time_point startedAt() const noexcept { return started_at_; }

    void start(std::optional<int32_t> trackIndex, std::optional<int32_t> clipId) noexcept {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        try {
            started_at_ = std::chrono::steady_clock::now();
            processed_.store(0, std::memory_order_release);
            total_.store(0, std::memory_order_release);
            if (worker_.joinable())
                worker_.join();
            stop_requested_.store(false, std::memory_order_release);
            worker_ = std::thread([this, trackIndex, clipId] {
                transcriber_.run(trackIndex, clipId, stop_requested_, processed_, total_);
                running_.store(false, std::memory_order_release);
            });
        } catch (...) {
            running_.store(false, std::memory_order_release);
        }
    }

    void stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        // Only once the worker is gone, so nothing is queuing edits any more.
        transcriber_.revokeQueuedEdits();
        running_.store(false, std::memory_order_release);
    }

private:
    Transcriber transcriber_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> processed_{0};
    std::atomic<int> total_{0};
    std::chrono::steady_clock::time_point started_at_{};
};

std::string progressSuffix(const TranscriptionJob& job) {
    try {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - job.startedAt()).count();
        const auto total = job.total();
        if (total > 0)
            return std::format(" ({}/{}; {}s)", job.processed(), total, elapsed);
        return std::format(" (starting; {}s)", elapsed);
    } catch (...) {
        return " (running...)";
    }
}

// While a run is in flight the command cancels it, so the label says so --
// still carrying the elapsed time, which is the part that tells the user
// anything is happening at all.
std::string commandLabel(std::string_view base, const TranscriptionJob& job) {
    if (!job.running())
        return std::string(base);
    if (job.cancelling()) {
        try {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - job.startedAt()).count();
            return std::format("Cancelling {} ({}s)", base, elapsed);
        } catch (...) {
            return std::format("Cancelling {}", base);
        }
    }
    return std::format("Cancel {}{}", base, progressSuffix(job));
}

class TranscribeAllCommand final : public Command {
public:
    explicit TranscribeAllCommand(TranscriptionJob& job) : job_(job) {}

    std::string_view id() const noexcept override {
        return "uapmd-pitch.transcribe-all-audio-clips";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{
            "Transcribe all audio clips to mono MIDI2 clip (pitch-detection)"};
        static thread_local std::string label;
        label = commandLabel(base, job_);
        return label;
    }

    int order() const noexcept override {
        return 1100;
    }

    bool enabled() const noexcept override {
        // Never greyed out: while running, activating it cancels.
        return true;
    }

    void invoke() noexcept override {
        if (job_.running()) {
            job_.requestStop();
            return;
        }
        job_.start(std::nullopt, std::nullopt);
    }

private:
    TranscriptionJob& job_;
};

class TranscribeClipCommand final : public ClipCommand {
public:
    explicit TranscribeClipCommand(TranscriptionJob& job) : job_(job) {}

    std::string_view id() const noexcept override {
        return "uapmd-pitch.transcribe-audio-clip";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{"Transcribe to mono MIDI2 clip (pitch-detection)"};
        static thread_local std::string label;
        label = commandLabel(base, job_);
        return label;
    }

    int order() const noexcept override {
        return 100;
    }

    bool appliesTo(const ClipCommandTarget& target) const noexcept override {
        return !target.midi_clip && !target.master_track;
    }

    bool enabled(const ClipCommandTarget&) const noexcept override {
        // Never greyed out: while running, activating it cancels.
        return true;
    }

    void invoke(const ClipCommandTarget& target) noexcept override {
        if (job_.running()) {
            job_.requestStop();
            return;
        }
        job_.start(target.track_index, target.clip_id);
    }

private:
    TranscriptionJob& job_;
};

class PitchTranscriptionAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/mir", "pitch-transcription"};
    }

    std::string_view name() const noexcept override {
        return "Audio to MIDI 2.0 pitch transcription";
    }

    std::string_view path() const noexcept override {
        return kCommandExtensionPoint;
    }

    bool initialize(AddinHost& host) noexcept override {
        auto* engine = static_cast<uapmd::SequencerEngine*>(
            host.extensionPoint(kEngineExtensionPoint));
        command_registry_ = static_cast<CommandRegistry*>(
            host.extensionPoint(kCommandExtensionPoint));
        clip_command_registry_ = static_cast<ClipCommandRegistry*>(
            host.extensionPoint(kClipCommandExtensionPoint));
        if (!engine || !command_registry_)
            return false;

        try {
            job_ = std::make_unique<TranscriptionJob>(*engine);
            all_command_ = std::make_unique<TranscribeAllCommand>(*job_);
            command_registry_->registerCommand(*all_command_);
            // The clip context menu is a host affordance; a host that does not
            // offer one simply leaves this extension point unregistered.
            if (clip_command_registry_) {
                clip_command_ = std::make_unique<TranscribeClipCommand>(*job_);
                clip_command_registry_->registerCommand(*clip_command_);
            }
            return true;
        } catch (...) {
            // The application command may already be registered by the time
            // the clip command fails to construct, so unwind the same way a
            // normal shutdown does rather than dropping a live registration.
            teardown();
            return false;
        }
    }

    void cleanup(AddinHost&) noexcept override {
        teardown();
    }

private:
    void teardown() noexcept {
        if (command_registry_ && all_command_)
            command_registry_->unregisterCommand(*all_command_);
        if (clip_command_registry_ && clip_command_)
            clip_command_registry_->unregisterCommand(*clip_command_);
        // Joins the worker before anything it runs in can be unloaded.
        if (job_)
            job_->stop();
        cleanupState();
    }

    void cleanupState() noexcept {
        all_command_.reset();
        clip_command_.reset();
        job_.reset();
        command_registry_ = nullptr;
        clip_command_registry_ = nullptr;
    }

    CommandRegistry* command_registry_{};
    ClipCommandRegistry* clip_command_registry_{};
    std::unique_ptr<TranscriptionJob> job_;
    std::unique_ptr<TranscribeAllCommand> all_command_;
    std::unique_ptr<TranscribeClipCommand> clip_command_;
};

PitchTranscriptionAddin pitchTranscriptionAddin;

} // namespace

// Assembled into the library's AddinEntry by AddinEntry.cpp. Unlike the
// analysis addins beside it, this one is compiled in unconditionally.
uapmd_addin::Addin* uapmd_pitch_transcription_addin() noexcept {
    return &pitchTranscriptionAddin;
}
