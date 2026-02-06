#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <format>

namespace remidy {
    enum class AudioContentType {
        Float32,
        Float64
    };

    // Represents a sample-accurate sequence of UMPs and/or alike.
    // It is part of `AudioProcessingContext`.
    class EventSequence {
        size_t allocated_buffer_size_bytes;
        void* messages{};
        size_t position_in_bytes{0};
    public:
        explicit EventSequence(size_t allocatedBufferSizeBytes) : allocated_buffer_size_bytes(allocatedBufferSizeBytes) {
            messages = (remidy_ump_t*) calloc(allocated_buffer_size_bytes, sizeof(uint8_t));
        }
        ~EventSequence() {
            free(messages);
        }
        size_t maxMessagesInBytes() { return allocated_buffer_size_bytes; }
        void* getMessages() { return messages; }
        size_t position() const { return position_in_bytes; }
        void position(size_t n) { position_in_bytes = n; }
    };

    class MasterContext {
        AudioContentType audio_data_type{AudioContentType::Float32};
        uint16_t dctpq{480};
        uint32_t tempo_{500000};
        int64_t playback_position_samples_{0};
        int32_t sample_rate_{48000};
        bool is_playing_{false};
        int32_t time_signature_numerator_{4};
        int32_t time_signature_denominator_{4};

    public:
        AudioContentType audioDataType() { return audio_data_type; }
        StatusCode audioDataType(AudioContentType newAudioDataType) {
            audio_data_type = newAudioDataType;
            return StatusCode::OK;
        }

        uint16_t deltaClockstampTicksPerQuarterNotes() { return dctpq; }
        StatusCode deltaClockstampTicksPerQuarterNotes(uint16_t newValue) {
            dctpq = newValue;
            return StatusCode::OK;
        }

        uint16_t tempo() {
            return tempo_;
        }
        StatusCode tempo(uint16_t newValue) {
            tempo_ = newValue;
            return StatusCode::OK;
        }
        int64_t playbackPositionSamples() const { return playback_position_samples_; }
        void playbackPositionSamples(int64_t newValue) { playback_position_samples_ = newValue; }

        int32_t sampleRate() const { return sample_rate_; }
        void sampleRate(int32_t newValue) { sample_rate_ = newValue; }

        bool isPlaying() const { return is_playing_; }
        void isPlaying(bool newValue) { is_playing_ = newValue; }

        int32_t timeSignatureNumerator() const { return time_signature_numerator_; }
        void timeSignatureNumerator(int32_t newValue) { time_signature_numerator_ = newValue; }

        int32_t timeSignatureDenominator() const { return time_signature_denominator_; }
        void timeSignatureDenominator(int32_t newValue) { time_signature_denominator_ = newValue; }
    };

    class TrackContext {
        MasterContext& master_context;
        // in case they are overriden...
        std::optional<uint16_t> dctpq_override{};
        std::optional<uint32_t> tempo_override{};

    public:
        explicit TrackContext(MasterContext& masterContext) :
            master_context(masterContext) {

        }

        MasterContext& masterContext() { return master_context; }

        uint16_t deltaClockstampTicksPerQuarterNotes() {
            return dctpq_override.has_value() ? dctpq_override.value() : master_context.deltaClockstampTicksPerQuarterNotes();
        }
        StatusCode deltaClockstampTicksPerQuarterNotes(uint16_t newValue) {
            dctpq_override = newValue;
            return StatusCode::OK;
        }

        uint16_t tempo() {
            return tempo_override.has_value() ? tempo_override.value() : master_context.tempo();
        }
        StatusCode tempo(uint16_t newValue) {
            tempo_override = newValue;
            return StatusCode::OK;
        }

        double ppqPosition() {
            // Calculate PPQ from samples, like VST3 does
            // PPQ = (samples / sampleRate) * (tempo_bpm / 60)
            double tempoBPM = 60000000.0 / master_context.tempo();
            double seconds = static_cast<double>(master_context.playbackPositionSamples()) /
                           master_context.sampleRate();
            return (seconds * tempoBPM) / 60.0;
        }
    };

    // Represents a set of realtime-safe input and output of audio processing.
    // Anything that is NOT RT-safe manipulation has to be done outside of this structure.
    class AudioProcessContext {

        // FIXME: remove this class and manage the entire audio buffers in one single array.
        class AudioBusBufferList {
            uint32_t channel_count{};
            uint32_t frame_capacity{};
            uint32_t owned_channel_count{};
            uint32_t owned_frame_capacity{};
            void* owned_data{nullptr};
            void* data_view{nullptr};
            bool aliasing{false};

        public:
            AudioBusBufferList(uint32_t channelCount, uint32_t bufferSizeInFrames) :
                    channel_count(channelCount),
                    frame_capacity(bufferSizeInFrames),
                    owned_channel_count(channelCount),
                    owned_frame_capacity(bufferSizeInFrames) {
                const uint32_t channels = channelCount > 0 ? channelCount : 1;
                const uint32_t frames = bufferSizeInFrames > 0 ? bufferSizeInFrames : 1;
                owned_data = calloc(sizeof(double) * frames, channels);
                data_view = owned_data;
            }
            ~AudioBusBufferList() {
                free(owned_data);
            }

            void clear() {
                if (!data_view || aliasing)
                    return;
                std::memset(data_view, 0, channel_count * frame_capacity * sizeof(double));
            }

            void aliasFrom(AudioBusBufferList& other) {
                aliasing = true;
                data_view = other.data_view;
                // Restrict to matching dimensions to avoid overruns
                channel_count = std::min(owned_channel_count, other.channel_count);
                frame_capacity = std::min(owned_frame_capacity, other.frame_capacity);
            }

            void useOwnedData() {
                if (!aliasing)
                    return;
                aliasing = false;
                data_view = owned_data;
                channel_count = owned_channel_count;
                frame_capacity = owned_frame_capacity;
            }

            void channelCount(uint32_t newCount) {
                if (newCount == owned_channel_count)
                    return;
                const uint32_t frames = owned_frame_capacity > 0 ? owned_frame_capacity : 1;
                const uint32_t channels = newCount > 0 ? newCount : 1;
                auto* newData = calloc(sizeof(double) * frames, channels);
                if (owned_data) {
                    auto toCopy = std::min(owned_channel_count, newCount);
                    if (toCopy > 0)
                        std::memcpy(newData, owned_data, sizeof(double) * frames * toCopy);
                    free(owned_data);
                }
                owned_data = newData;
                owned_channel_count = newCount;
                if (!aliasing) {
                    channel_count = newCount;
                    data_view = owned_data;
                }
            }

            float* getFloatBufferForChannel(uint32_t channel) const {
                return channel >= channel_count ? nullptr : static_cast<float *>(data_view) + channel * frame_capacity;
            }
            double* getDoubleBufferForChannel(uint32_t channel) const {
                return channel >= channel_count ? nullptr : static_cast<double *>(data_view) + channel * frame_capacity;
            };
            uint32_t channelCount() const { return channel_count; }
            uint32_t bufferCapacityInFrames() const { return frame_capacity; }
        };

        TrackContext track_context;
        std::vector<AudioBusBufferList*> audio_in{};
        std::vector<AudioBusBufferList*> audio_out{};
        EventSequence event_in;
        EventSequence event_out;
        int32_t frame_count{0};
        size_t audio_buffer_capacity_frames;
        bool replacing_enabled_{false};

    public:
        AudioProcessContext(
            MasterContext& masterContext,
            const uint32_t eventBufferSizeBytes
        ) : track_context(masterContext),
            event_in(eventBufferSizeBytes),
            event_out(eventBufferSizeBytes),
            audio_buffer_capacity_frames(0) {
        }
        ~AudioProcessContext() {
            for (const auto bus : audio_in)
                if (bus)
                    delete bus;
            for (const auto bus : audio_out)
                if (bus)
                    delete bus;
        }

        // FIXME: there should be configure() with full list of bus configurations

        void configureMainBus(int32_t inChannels, int32_t outChannels, size_t audioBufferCapacityInFrames) {
            audio_buffer_capacity_frames = audioBufferCapacityInFrames;
            audio_in.emplace_back(new AudioBusBufferList(inChannels, audioBufferCapacityInFrames));
            audio_out.emplace_back(new AudioBusBufferList(outChannels, audioBufferCapacityInFrames));
        }

        void addAudioIn(int32_t channels, size_t audioBufferCapacityInFrames) {
            if (audioBufferCapacityInFrames > audio_buffer_capacity_frames)
                audio_buffer_capacity_frames = audioBufferCapacityInFrames;
            audio_in.emplace_back(new AudioBusBufferList(channels, audioBufferCapacityInFrames));
        }

        void addAudioOut(int32_t channels, size_t audioBufferCapacityInFrames) {
            if (audioBufferCapacityInFrames > audio_buffer_capacity_frames)
                audio_buffer_capacity_frames = audioBufferCapacityInFrames;
            audio_out.emplace_back(new AudioBusBufferList(channels, audioBufferCapacityInFrames));
        }

        TrackContext* trackContext() { return &track_context; }

        size_t audioBufferCapacityInFrames() { return audio_buffer_capacity_frames; }

        int32_t frameCount() const { return frame_count; }
        void frameCount(const int32_t newCount) { frame_count = newCount; }


        int32_t audioInBusCount() const { return static_cast<int32_t>(audio_in.size()); }
        int32_t audioOutBusCount() const { return static_cast<int32_t>(audio_out.size()); }

        int32_t inputChannelCount(int32_t bus) const { return bus >= audioInBusCount() ? 0 : audio_in[bus]->channelCount(); }
        int32_t outputChannelCount(int32_t bus) const { return bus >= audioOutBusCount() ? 0 : audio_out[bus]->channelCount(); }

        float* getFloatInBuffer(int32_t bus, uint32_t channel) const {
            if (bus < 0 || bus >= audioInBusCount())
                return nullptr;
            auto b = audio_in[bus];
            return channel >= b->channelCount() ? nullptr : b->getFloatBufferForChannel(channel);
        }
        float* getFloatOutBuffer(int32_t bus, uint32_t channel) const {
            if (bus < 0 || bus >= audioOutBusCount())
                return nullptr;
            auto b = audio_out[bus];
            return channel >= b->channelCount() ? nullptr : b->getFloatBufferForChannel(channel);
        }
        double* getDoubleInBuffer(int32_t bus, uint32_t channel) const {
            if (bus < 0 || bus >= audioInBusCount())
                return nullptr;
            auto b = audio_in[bus];
            return channel >= b->channelCount() ? nullptr : b->getDoubleBufferForChannel(channel);
        };
        double* getDoubleOutBuffer(int32_t bus, uint32_t channel) const {
            if (bus < 0 || bus >= audioOutBusCount())
                return nullptr;
            auto b = audio_out[bus];
            return channel >= b->channelCount() ? nullptr : b->getDoubleBufferForChannel(channel);
        };

        void copyInputsToOutputs() {
            auto dataType = track_context.masterContext().audioDataType();
            size_t busCount = std::min(audio_in.size(), audio_out.size());
            for (size_t i = 0; i < busCount; ++i) {
                auto* inBus = audio_in[i];
                auto* outBus = audio_out[i];
                if (!inBus || !outBus)
                    continue;
                auto channels = std::min(inBus->channelCount(), outBus->channelCount());
                auto frames = std::min(inBus->bufferCapacityInFrames(), outBus->bufferCapacityInFrames());
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    if (dataType == AudioContentType::Float64) {
                        auto* src = inBus->getDoubleBufferForChannel(ch);
                        auto* dst = outBus->getDoubleBufferForChannel(ch);
                        if (src && dst)
                            std::memcpy(dst, src, frames * sizeof(double));
                    } else {
                        auto* src = inBus->getFloatBufferForChannel(ch);
                        auto* dst = outBus->getFloatBufferForChannel(ch);
                        if (src && dst)
                            std::memcpy(dst, src, frames * sizeof(float));
                    }
                }
            }
        }

        void enableReplacingIO() {
            if (replacing_enabled_)
                return;
            size_t busCount = std::min(audio_in.size(), audio_out.size());
            for (size_t i = 0; i < busCount; ++i) {
                if (audio_in[i] && audio_out[i])
                    audio_in[i]->aliasFrom(*audio_out[i]);
            }
            replacing_enabled_ = true;
        }

        void disableReplacingIO() {
            if (!replacing_enabled_)
                return;
            for (auto* bus : audio_in)
                if (bus)
                    bus->useOwnedData();
            replacing_enabled_ = false;
        }

        EventSequence& eventIn() { return event_in; }
        EventSequence& eventOut() { return event_out; }

        void clearAudioOutputs() {
            for (auto* bus : audio_out)
                if (bus)
                    bus->clear();
            event_out.position(0);
        }

        void advanceToNextNode() {
            auto dataType = track_context.masterContext().audioDataType();
            // Copy audio output to input for the next node
            for (size_t i = 0; i < std::min(audio_in.size(), audio_out.size()); ++i) {
                auto* inBus = audio_in[i];
                auto* outBus = audio_out[i];
                if (inBus && outBus) {
                    auto channels = std::min(inBus->channelCount(), outBus->channelCount());
                    auto frames = std::min(inBus->bufferCapacityInFrames(), outBus->bufferCapacityInFrames());
                    for (uint32_t ch = 0; ch < channels; ++ch) {
                        if (dataType == AudioContentType::Float64) {
                            auto* dst = inBus->getDoubleBufferForChannel(ch);
                            auto* src = outBus->getDoubleBufferForChannel(ch);
                            if (dst && src)
                                std::memcpy(dst, src, frames * sizeof(double));
                        } else {
                            auto* dst = inBus->getFloatBufferForChannel(ch);
                            auto* src = outBus->getFloatBufferForChannel(ch);
                            if (dst && src)
                                std::memcpy(dst, src, frames * sizeof(float));
                        }
                    }
                }
            }

            // Clear output buffers for the next plugin
            for (auto* bus : audio_out)
                if (bus)
                    bus->clear();

            event_out.position(0);
        }
    };

}
