#include <array>
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

#include "BasicPitchTranscriber.hpp"
#include "ClipTranscription.hpp"
#include "NoteClipWriter.hpp"

using namespace uapmd_addin;

namespace {

constexpr std::string_view kCommandExtensionPoint{"/uapmd/app/command/v1"};
constexpr std::string_view kClipCommandExtensionPoint{"/uapmd/app/clip-command/v1"};
constexpr std::string_view kEngineExtensionPoint{"/uapmd/engine/v1"};

// The model's contour head resolves three bins per semitone, so a bend unit is
// a third of a semitone.
constexpr double kSemitonesPerBendUnit = 1.0 / 3.0;

// Converts the model's frame-indexed note events into the representation the
// clip writer takes: seconds, a MIDI note number, a velocity, and the pitch
// curve the contour head measured.
std::vector<uapmd_pitch::TranscribedNote> toTranscribedNotes(
        const std::vector<uapmd_basic_pitch::NoteEvent>& events) {
    std::vector<uapmd_pitch::TranscribedNote> notes;
    notes.reserve(events.size());
    for (const auto& event : events) {
        const auto start = uapmd_basic_pitch::frameToSeconds(event.start_frame);
        const auto end = uapmd_basic_pitch::frameToSeconds(event.end_frame);
        if (end <= start)
            continue;

        uapmd_pitch::TranscribedNote note;
        note.start_seconds = start;
        note.end_seconds = end;
        note.note_number = static_cast<uint8_t>(std::clamp(event.pitch_midi, 0, 127));
        note.velocity = std::clamp(event.amplitude, 0.0f, 1.0f);

        // The first bend point doubles as the note's own detected pitch, so a
        // receiver that ignores per-note bend still lands on the right cents.
        double firstOffset = 0.0;
        note.bend.reserve(event.bends.size());
        for (size_t index = 0; index < event.bends.size(); ++index) {
            const auto offset = event.bends[index] * kSemitonesPerBendUnit;
            if (index == 0)
                firstOffset = offset;
            note.bend.push_back({
                uapmd_basic_pitch::frameToSeconds(event.start_frame + static_cast<int>(index)) - start,
                offset});
        }
        note.detected_semitones = event.pitch_midi + firstOffset;
        notes.push_back(std::move(note));
    }
    return notes;
}

// Everything the two commands share, minus how the clips were chosen. Runs on a
// worker thread; `stopRequested` lets a shutdown cut it short.
class Transcriber {
public:
    explicit Transcriber(uapmd::SequencerEngine& engine) : engine_(engine) {}

    // Returns how many MIDI clips were added.
    int run(std::optional<int32_t> trackIndex,
            std::optional<int32_t> clipId,
            const std::atomic<bool>& stopRequested,
            std::atomic<int>& processed,
            std::atomic<int>& total) noexcept {
        int added = 0;
        try {
            // Reading the weights is deferred to the first run rather than done
            // at initialize(): parsing 230 KB is cheap, but a project that never
            // asks for transcription should not pay for it at startup.
            if (!weights_loaded_) {
                std::string error;
                if (!uapmd_basic_pitch::loadModelWeights(
                        uapmd_basic_pitch::embeddedModel(), weights_, error)) {
                    remidy::Logger::global()->logError(std::format(
                        "Basic Pitch model could not be read: {}", error).c_str());
                    return 0;
                }
                weights_loaded_ = true;
            }

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

                // The network was trained at one rate and one rate only.
                const auto resampled = uapmd_pitch::resample(
                    mono, audio.source.sampleRate, uapmd_basic_pitch::kSampleRate);

                uapmd_basic_pitch::Posteriorgrams posteriorgrams;
                if (!uapmd_basic_pitch::computePosteriorgrams(
                        weights_, resampled, posteriorgrams,
                        [&stopRequested](double) {
                            return !stopRequested.load(std::memory_order_acquire);
                        }))
                    break;

                const auto events = uapmd_basic_pitch::decodeNotes(
                    posteriorgrams, uapmd_basic_pitch::BasicPitchOptions{});
                if (uapmd_pitch::writeNoteClip(
                        engine_, audio, toTranscribedNotes(events), "Basic Pitch"))
                    ++added;
            }
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError(std::format(
                "Basic Pitch transcription failed: {}", error.what()).c_str());
        } catch (...) {
            remidy::Logger::global()->logError("Basic Pitch transcription failed");
        }
        return added;
    }

private:
    uapmd::SequencerEngine& engine_;
    uapmd_basic_pitch::ModelWeights weights_;
    bool weights_loaded_{false};
};

// One worker thread shared by both commands, so the per-clip and whole-project
// entry points cannot run over each other.
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
        return "uapmd-basic-pitch.transcribe-all-audio-clips";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{
            "Transcribe all audio clips to poly MIDI2 clip (basic-pitch)"};
        static thread_local std::string label;
        label = commandLabel(base, job_);
        return label;
    }

    int order() const noexcept override { return 1200; }
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
        return "uapmd-basic-pitch.transcribe-audio-clip";
    }

    std::string_view title() const noexcept override {
        static constexpr std::string_view base{"Transcribe to poly MIDI2 clip (basic-pitch)"};
        static thread_local std::string label;
        label = commandLabel(base, job_);
        return label;
    }

    int order() const noexcept override { return 101; }

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

class BasicPitchAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/basic-pitch", "transcription"};
    }

    std::string_view name() const noexcept override {
        return "Basic Pitch polyphonic transcription";
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
            // The clip context menu is a host affordance; a host without one
            // simply leaves this extension point unregistered.
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
        // Joins the worker before anything it runs in can be unloaded.
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

BasicPitchAddin basicPitchAddin;

class BasicPitchAddinEntry final : public AddinEntry {
public:
    BasicPitchAddinEntry() { addins_[0] = &basicPitchAddin; }

    std::string_view packageId() const noexcept override {
        return "/uapmd/basic-pitch";
    }

    std::span<Addin* const> addins() noexcept override { return addins_; }

private:
    std::array<Addin*, 1> addins_{};
};

BasicPitchAddinEntry basicPitchAddinEntry;

// Built-in addin, like the stem separators: linked into the application rather
// than loaded from the addin directory, which is what makes it available on
// WebAssembly, Android and iOS where dynamic addin loading is unavailable.
class BasicPitchBuiltinAddinRegistration final {
public:
    BasicPitchBuiltinAddinRegistration() {
        registerBuiltinAddin(basicPitchAddinEntry);
    }
};

BasicPitchBuiltinAddinRegistration basicPitchBuiltinAddinRegistration;

} // namespace
