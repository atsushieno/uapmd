#include <array>
#include <atomic>
#include <chrono>
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
#include "DrumScriptTranscriber.hpp"
#include "MainThreadTimelineEdit.hpp"

using namespace uapmd_addin;

namespace {

constexpr std::string_view kCommandExtensionPoint{"/uapmd/app/project-command/v1"};
constexpr std::string_view kClipCommandExtensionPoint{"/uapmd/app/clip-command/v1"};
constexpr std::string_view kEngineExtensionPoint{"/uapmd/engine/v1"};

// Runs on a worker thread; `stopRequested` lets a shutdown cut it short.
class Transcriber {
public:
    explicit Transcriber(uapmd::SequencerEngine& engine) : engine_(engine) {}

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

            for (const auto& audio : sources) {
                if (stopRequested.load(std::memory_order_acquire))
                    break;
                processed.fetch_add(1, std::memory_order_acq_rel);

                std::vector<float> mono;
                if (!uapmd_pitch::readMono(view, audio.source, mono))
                    continue;

                // The thresholds were measured at this rate and the spectral
                // ones depend on it, so the signal comes to them, not the
                // other way round.
                const auto resampled = uapmd_pitch::resample(
                    mono, audio.source.sampleRate, uapmd_drumscript::kAnalysisSampleRate);

                const auto notes = uapmd_drumscript::transcribeDrums(
                    resampled, uapmd_drumscript::kAnalysisSampleRate, thresholds_,
                    [&stopRequested](double) {
                        return !stopRequested.load(std::memory_order_acquire);
                    });
                if (notes.empty())
                    continue;

                uapmd_pitch::writeNoteClip(
                    engine_, edit_lifetime_, audio, notes, "DrumScript");
            }
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "DrumScript transcription failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("DrumScript transcription failed");
        }
    }

private:
    uapmd::SequencerEngine& engine_;
    uapmd_drumscript::DrumThresholds thresholds_{};
    uapmd_mir::AsyncEditLifetimeRef edit_lifetime_{uapmd_mir::makeAsyncEditLifetime()};
};

// One worker shared by both commands, so the per-clip and whole-project entry
// points cannot run over each other.
class TranscriptionJob {
public:
    explicit TranscriptionJob(uapmd::SequencerEngine& engine) : transcriber_(engine) {}

    ~TranscriptionJob() { stop(); }

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    bool cancelling() const noexcept {
        return running() && stop_requested_.load(std::memory_order_acquire);
    }
    void requestStop() noexcept { stop_requested_.store(true, std::memory_order_release); }
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
        return "uapmd-drumscript.transcribe-all-audio-clips";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{
            "Transcribe all audio clips to drum MIDI2 clip (DrumScript)"};
        static thread_local std::string label;
        label = commandLabel(base, job_);
        return label;
    }

    int order() const noexcept override { return 1210; }
    bool enabled() const noexcept override { return true; }

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
        return "uapmd-drumscript.transcribe-audio-clip";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{"Transcribe to drum MIDI2 clip (DrumScript)"};
        static thread_local std::string label;
        label = commandLabel(base, job_);
        return label;
    }

    int order() const noexcept override { return 102; }

    bool appliesTo(const ClipCommandTarget& target) const noexcept override {
        return !target.midi_clip && !target.master_track;
    }

    bool enabled(const ClipCommandTarget&) const noexcept override { return true; }

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

class DrumScriptAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/drumscript", "transcription"};
    }

    std::string_view name() const noexcept override {
        return "DrumScript drum transcription";
    }

    std::string_view path() const noexcept override { return kCommandExtensionPoint; }

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
            if (clip_command_registry_) {
                clip_command_ = std::make_unique<TranscribeClipCommand>(*job_);
                clip_command_registry_->registerCommand(*clip_command_);
            }
            return true;
        } catch (...) {
            teardown();
            return false;
        }
    }

    void cleanup(AddinHost&) noexcept override { teardown(); }

private:
    void teardown() noexcept {
        if (command_registry_ && all_command_)
            command_registry_->unregisterCommand(*all_command_);
        if (clip_command_registry_ && clip_command_)
            clip_command_registry_->unregisterCommand(*clip_command_);
        if (job_)
            job_->stop();
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

DrumScriptAddin drumScriptAddin;

class DrumScriptAddinEntry final : public AddinEntry {
public:
    DrumScriptAddinEntry() { addins_[0] = &drumScriptAddin; }

    std::string_view packageId() const noexcept override { return "/uapmd/drumscript"; }

    std::span<Addin* const> addins() noexcept override { return addins_; }

private:
    std::array<Addin*, 1> addins_{};
};

DrumScriptAddinEntry drumScriptAddinEntry;

// Built-in addin, like Basic Pitch and the stem separators: linked into the
// application rather than loaded from the addin directory, which is what makes
// it available on WebAssembly, Android and iOS.
class DrumScriptBuiltinAddinRegistration final {
public:
    DrumScriptBuiltinAddinRegistration() { registerBuiltinAddin(drumScriptAddinEntry); }
};

DrumScriptBuiltinAddinRegistration drumScriptBuiltinAddinRegistration;

} // namespace
