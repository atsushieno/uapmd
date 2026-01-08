#include "uapmd/uapmd.hpp"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace uapmd {

    class SequencerEngineImpl : public SequencerEngine {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        uint32_t default_input_channels_{2};
        uint32_t default_output_channels_{2};
        std::vector<std::unique_ptr<AudioPluginTrack>> tracks_{};
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        AudioPluginHostingAPI* pal;

        // Audio file playback
        std::unique_ptr<AudioFileReader> audio_file_reader_;
        std::vector<std::vector<float>> audio_file_buffer_; // per-channel buffers
        std::atomic<size_t> audio_file_read_position_{0};
        mutable std::mutex audio_file_mutex_;

        // Playback state (managed by AudioPluginSequencer)
        std::atomic<bool> is_playback_active_{false};
        std::atomic<int64_t> playback_position_samples_{0};

        // Merged input buffer (reused across process calls)
        std::vector<std::vector<float>> merged_input_;

        // Audio analysis
        static constexpr int kSpectrumBars = 32;
        // RT-thread local buffers (no lock needed)
        float rt_input_spectrum_[kSpectrumBars] = {};
        float rt_output_spectrum_[kSpectrumBars] = {};
        // Shared buffers for non-RT readers (lock-free using atomic flag)
        float shared_input_spectrum_[kSpectrumBars] = {};
        float shared_output_spectrum_[kSpectrumBars] = {};
        // Lock-free flag: true = reader owns, false = writer can write
        mutable std::atomic<bool> spectrum_reading_{false};

    public:
        explicit SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal);

        SequenceProcessContext& data() override { return sequence; }

        std::vector<AudioPluginTrack*>& tracks() const override;

        void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
        void addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, AudioPluginTrack* track, int32_t trackIndex, std::string error)> callback) override;
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, std::string error)> callback) override;
        bool removePluginInstance(int32_t instanceId) override;

        uapmd_status_t processAudio(AudioProcessContext& process) override;

        // Playback control
        void setPlaybackActive(bool active) override;
        bool isPlaybackActive() const override;
        void setPlaybackPosition(int64_t samples) override;
        int64_t playbackPosition() const override;

        // Audio file playback
        void loadAudioFile(std::unique_ptr<AudioFileReader> reader) override;
        void unloadAudioFile() override;
        double audioFileDurationSeconds() const override;

        // Audio analysis
        void getInputSpectrum(float* outSpectrum, int numBars) const override;
        void getOutputSpectrum(float* outSpectrum, int numBars) const override;

    private:
        void removeTrack(size_t index);
    };

    std::unique_ptr<SequencerEngine> SequencerEngine::create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal) {
        return std::make_unique<SequencerEngineImpl>(sampleRate, audioBufferSizeInFrames, umpBufferSizeInInts, pal);
    }

    // SequencerEngineImpl
    SequencerEngineImpl::SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        pal(pal) {
    }

    std::vector<AudioPluginTrack*> &SequencerEngineImpl::tracks() const {
        // Note: This requires a mutable cache for const correctness
        // Since we need to return a reference to a vector of raw pointers
        static thread_local std::vector<AudioPluginTrack*> track_ptrs;
        track_ptrs.clear();
        for (const auto& track : tracks_)
            track_ptrs.push_back(track.get());
        return track_ptrs;
    }

    int32_t SequencerEngineImpl::processAudio(AudioProcessContext& process) {
        if (tracks_.size() != sequence.tracks.size())
            // FIXME: define status codes
            return 1;

        auto& data = sequence;
        auto& masterContext = data.masterContext();

        // Update playback position if playback is active
        bool isPlaybackActive = is_playback_active_.load(std::memory_order_acquire);

        // Update MasterContext with current playback state
        masterContext.playbackPositionSamples(playback_position_samples_.load(std::memory_order_acquire));
        masterContext.isPlaying(isPlaybackActive);
        masterContext.sampleRate(sampleRate);

        // Prepare merged input buffer (audio file + mic input)
        size_t audioFilePosition = 0;
        bool hasAudioFile = false;

        {
            std::lock_guard<std::mutex> lock(audio_file_mutex_);
            hasAudioFile = !audio_file_buffer_.empty();
            if (hasAudioFile && isPlaybackActive) {
                audioFilePosition = audio_file_read_position_.load(std::memory_order_acquire);
            }
        }

        // Determine number of channels for merged input
        // Use the maximum of device input channels and audio file channels
        uint32_t deviceInputChannels = process.audioInBusCount() > 0 ? process.inputChannelCount(0) : 0;
        uint32_t fileChannels = hasAudioFile ? audio_file_buffer_.size() : 0;
        uint32_t numInputChannels = std::max(deviceInputChannels, fileChannels);

        // If both are 0, default to stereo
        if (numInputChannels == 0) {
            numInputChannels = 2;
        }

        merged_input_.resize(numInputChannels);
        for (auto& channel : merged_input_) {
            channel.assign(process.frameCount(), 0.0f);
        }

        // Fill merged input buffer
        for (uint32_t ch = 0; ch < numInputChannels; ch++) {
            float* dst = merged_input_[ch].data();

            // Start with device input (mic)
            if (process.audioInBusCount() > 0 && ch < process.inputChannelCount(0)) {
                memcpy(dst, (void*)process.getFloatInBuffer(0, ch), process.frameCount() * sizeof(float));
            }

            // Add audio file playback if available and playing
            if (hasAudioFile && isPlaybackActive) {
                std::lock_guard<std::mutex> lock(audio_file_mutex_);
                if (ch < audio_file_buffer_.size()) {
                    const auto& channelData = audio_file_buffer_[ch];
                    for (uint32_t frame = 0; frame < process.frameCount(); ++frame) {
                        size_t pos = audioFilePosition + frame;
                        if (pos < channelData.size()) {
                            dst[frame] += channelData[pos];
                        }
                    }
                }
            }
        }

        // Send merged input to ALL tracks
        for (uint32_t t = 0, nTracks = tracks_.size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventOut().position(0); // clean up *out* events here.
            ctx->frameCount(process.frameCount());

            // Copy merged input to track input buffers
            for (uint32_t i = 0; i < ctx->audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->inputChannelCount(i); ch < nCh; ch++) {
                    float* trackDst = ctx->getFloatInBuffer(i, ch);
                    if (ch < merged_input_.size()) {
                        memcpy(trackDst, merged_input_[ch].data(), process.frameCount() * sizeof(float));
                    } else {
                        memset(trackDst, 0, process.frameCount() * sizeof(float));
                    }
                }
            }
        }

        // Advance audio file read position only when playing
        if (hasAudioFile && isPlaybackActive) {
            audio_file_read_position_.fetch_add(process.frameCount(), std::memory_order_release);
        }

        // Process all tracks
        for (auto i = 0; i < sequence.tracks.size(); i++) {
            auto& tp = *sequence.tracks[i];
            tracks_[i]->processAudio(tp);
            tp.eventIn().position(0); // reset
        }

        // Clear main output bus (bus 0) before mixing
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
            }
        }

        // Mix all tracks into main output bus with additive mixing
        for (uint32_t t = 0, nTracks = tracks_.size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.

            // Mix only main bus (bus 0)
            if (process.audioOutBusCount() > 0 && ctx->audioOutBusCount() > 0) {
                // Mix matching channels only
                uint32_t numChannels = std::min(ctx->outputChannelCount(0), process.outputChannelCount(0));
                for (uint32_t ch = 0; ch < numChannels; ch++) {
                    float* dst = process.getFloatOutBuffer(0, ch);
                    const float* src = ctx->getFloatOutBuffer(0, ch);
                    // Additive mixing
                    for (uint32_t frame = 0; frame < process.frameCount(); frame++) {
                        dst[frame] += src[frame];
                    }
                }
            }
        }

        // Apply soft clipping to prevent harsh distortion
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                float* buffer = process.getFloatOutBuffer(0, ch);
                for (uint32_t frame = 0; frame < process.frameCount(); frame++) {
                    buffer[frame] = std::tanh(buffer[frame]);
                }
            }
        }

        // Calculate spectrum for visualization (simple magnitude binning)
        // RT-safe: write to local buffers without locking
        {
            // Calculate input spectrum from merged input buffer
            for (int bar = 0; bar < kSpectrumBars; ++bar) {
                float sum = 0.0f;
                int sampleCount = 0;

                if (!merged_input_.empty()) {
                    int samplesPerBar = process.frameCount() / kSpectrumBars;
                    int startSample = bar * samplesPerBar;
                    int endSample = std::min((int)process.frameCount(), (bar + 1) * samplesPerBar);

                    for (uint32_t ch = 0; ch < merged_input_.size(); ++ch) {
                        const float* buffer = merged_input_[ch].data();
                        for (int i = startSample; i < endSample; ++i) {
                            sum += std::abs(buffer[i]);
                            sampleCount++;
                        }
                    }
                }

                rt_input_spectrum_[bar] = sampleCount > 0 ? sum / sampleCount : 0.0f;
            }

            // Calculate output spectrum from main output
            for (int bar = 0; bar < kSpectrumBars; ++bar) {
                float sum = 0.0f;
                int sampleCount = 0;

                if (process.audioOutBusCount() > 0) {
                    int samplesPerBar = process.frameCount() / kSpectrumBars;
                    int startSample = bar * samplesPerBar;
                    int endSample = std::min((int)process.frameCount(), (bar + 1) * samplesPerBar);

                    for (uint32_t ch = 0; ch < process.outputChannelCount(0); ++ch) {
                        const float* buffer = process.getFloatOutBuffer(0, ch);
                        for (int i = startSample; i < endSample; ++i) {
                            sum += std::abs(buffer[i]);
                            sampleCount++;
                        }
                    }
                }

                rt_output_spectrum_[bar] = sampleCount > 0 ? sum / sampleCount : 0.0f;
            }

            // Try to copy to shared buffers (skip if reader is active - RT-safe, lock-free)
            bool expected = false;
            if (spectrum_reading_.compare_exchange_strong(expected, false, std::memory_order_acquire)) {
                // No reader active, safe to write
                std::copy(rt_input_spectrum_, rt_input_spectrum_ + kSpectrumBars, shared_input_spectrum_);
                std::copy(rt_output_spectrum_, rt_output_spectrum_ + kSpectrumBars, shared_output_spectrum_);
                // No need to release the flag - we keep it at false for next write
            }
        }

        if (isPlaybackActive)
            playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);

        // FIXME: define status codes
        return 0;
    }

    void SequencerEngineImpl::setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) {
        default_input_channels_ = inputChannels;
        default_output_channels_ = outputChannels;
    }

    void SequencerEngineImpl::addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, AudioPluginTrack* track, int32_t trackIndex, std::string error)> callback) {
        pal->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this,callback](auto node, std::string error) {
            if (!node) {
                callback(nullptr, nullptr, -1, "Could not create simple track: " + error);
                return;
            }

            // Create track and add plugin
            {
                auto tr = AudioPluginTrack::create(ump_buffer_size_in_ints);
                tr->graph().appendNodeSimple(std::move(node));
                auto* track_ptr = tr.get();
                tracks_.emplace_back(std::move(tr));
                sequence.tracks.emplace_back(new AudioProcessContext(sequence.masterContext(), ump_buffer_size_in_ints));
            }
            auto* track = tracks_.back().get();
            auto trackIndex = static_cast<int32_t>(tracks_.size() - 1);

            // Configure main bus (moved from AudioPluginSequencer)
            auto trackCtx = sequence.tracks[trackIndex];
            trackCtx->configureMainBus(default_input_channels_, default_output_channels_, audio_buffer_size_in_frames);

            // Get the plugin node
            auto plugins = track->graph().plugins();
            if (plugins.empty()) {
                callback(nullptr, track, trackIndex, "Track has no plugins after instantiation");
                return;
            }

            track->bypassed(false);
            callback(plugins.front(), track, trackIndex, "");
        });
    }

    void SequencerEngineImpl::addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, std::string error)> callback) {
        // Validate track index
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
            callback(nullptr, std::format("Invalid track index {}", trackIndex));
            return;
        }

        pal->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this, trackIndex, callback](auto node, std::string error) {
            if (!node) {
                callback(nullptr, "Could not create plugin: " + error);
                return;
            }

            // Re-validate track (may have been removed during async operation)
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
                callback(nullptr, std::format("Track {} no longer exists", trackIndex));
                return;
            }

            auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
            auto* nodePtr = node.get();

            // Append to track's graph
            auto status = track->graph().appendNodeSimple(std::move(node));
            if (status != 0) {
                callback(nullptr, std::format("Failed to append plugin to track {} (status {})", trackIndex, status));
                return;
            }

            callback(nodePtr, "");
        });
    }

    bool SequencerEngineImpl::removePluginInstance(int32_t instanceId) {
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto& track = tracks_[i];
            if (!track)
                continue;
            if (track->graph().removePluginInstance(instanceId)) {
                if (track->graph().plugins().empty())
                    removeTrack(i);
                return true;
            }
        }
        return false;
    }

    void SequencerEngineImpl::removeTrack(size_t index) {
        if (index >= tracks_.size())
            return;
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        if (index < sequence.tracks.size()) {
            delete sequence.tracks[index];
            sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
        }
    }

    // Playback control
    void SequencerEngineImpl::setPlaybackActive(bool active) {
        is_playback_active_.store(active, std::memory_order_release);
    }

    bool SequencerEngineImpl::isPlaybackActive() const {
        return is_playback_active_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::setPlaybackPosition(int64_t samples) {
        playback_position_samples_.store(samples, std::memory_order_release);
    }

    int64_t SequencerEngineImpl::playbackPosition() const {
        return playback_position_samples_.load(std::memory_order_acquire);
    }

    // Audio file playback
    void SequencerEngineImpl::loadAudioFile(std::unique_ptr<AudioFileReader> reader) {
        if (!reader)
            return;

        std::lock_guard<std::mutex> lock(audio_file_mutex_);

        const auto& props = reader->getProperties();
        audio_file_buffer_.clear();
        audio_file_buffer_.resize(props.numChannels);

        // Read entire file into memory
        for (uint32_t ch = 0; ch < props.numChannels; ++ch) {
            audio_file_buffer_[ch].resize(props.numFrames);
        }

        // Prepare array of channel pointers for planar read
        std::vector<float*> destPtrs;
        destPtrs.reserve(props.numChannels);
        for (uint32_t ch = 0; ch < props.numChannels; ++ch) {
            destPtrs.push_back(audio_file_buffer_[ch].data());
        }

        // Read all frames into our planar buffers
        reader->readFrames(0, props.numFrames, destPtrs.data(), props.numChannels);

        audio_file_reader_ = std::move(reader);
        audio_file_read_position_.store(0, std::memory_order_release);
    }

    void SequencerEngineImpl::unloadAudioFile() {
        std::lock_guard<std::mutex> lock(audio_file_mutex_);
        audio_file_reader_.reset();
        audio_file_buffer_.clear();
        audio_file_read_position_.store(0, std::memory_order_release);
    }

    double SequencerEngineImpl::audioFileDurationSeconds() const {
        std::lock_guard<std::mutex> lock(audio_file_mutex_);
        if (!audio_file_reader_)
            return 0.0;
        const auto& props = audio_file_reader_->getProperties();
        return static_cast<double>(props.numFrames) / props.sampleRate;
    }

    // Audio analysis
    void SequencerEngineImpl::getInputSpectrum(float* outSpectrum, int numBars) const {
        // Set reading flag to prevent RT thread from writing
        spectrum_reading_.store(true, std::memory_order_release);

        for (int i = 0; i < std::min(numBars, kSpectrumBars); ++i) {
            outSpectrum[i] = shared_input_spectrum_[i];
        }

        // Release the reading flag
        spectrum_reading_.store(false, std::memory_order_release);
    }

    void SequencerEngineImpl::getOutputSpectrum(float* outSpectrum, int numBars) const {
        // Set reading flag to prevent RT thread from writing
        spectrum_reading_.store(true, std::memory_order_release);

        for (int i = 0; i < std::min(numBars, kSpectrumBars); ++i) {
            outSpectrum[i] = shared_output_spectrum_[i];
        }

        // Release the reading flag
        spectrum_reading_.store(false, std::memory_order_release);
    }

}
