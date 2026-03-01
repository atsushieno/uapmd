#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <format>

#include "port-extensibility.hpp"

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

        double ppqPosition() {
            // Calculate PPQ from samples, like VST3 does
            // PPQ = (samples / sampleRate) * (tempo_bpm / 60)
            double tempoBPM = 60000000.0 / tempo();
            double seconds = static_cast<double>(playbackPositionSamples()) / sampleRate();
            return (seconds * tempoBPM) / 60.0;
        }

        uint32_t tempo() {
            return tempo_;
        }
        StatusCode tempo(uint32_t newValue) {
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

    struct AudioBusSpec {
        AudioBusRole role{AudioBusRole::Main};
        uint32_t channels{0};
        size_t bufferCapacityFrames{0};
        bool operator==(const AudioBusSpec&) const = default;
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
            AudioBusRole bus_role{AudioBusRole::Main};

        public:
            AudioBusBufferList(uint32_t channelCount, uint32_t bufferSizeInFrames, AudioBusRole role = AudioBusRole::Main) :
                    channel_count(channelCount),
                    frame_capacity(bufferSizeInFrames),
                    owned_channel_count(channelCount),
                    owned_frame_capacity(bufferSizeInFrames),
                    bus_role(role) {
                const uint32_t channels = channelCount > 0 ? channelCount : 1;
                const uint32_t frames = bufferSizeInFrames > 0 ? bufferSizeInFrames : 1;
                // Allocate (frames * channels) elements of sizeof(double) bytes each
                owned_data = calloc(frames * channels, sizeof(double));
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
                bus_role = other.bus_role;
            }

            void useOwnedData() {
                if (!aliasing)
                    return;
                aliasing = false;
                data_view = owned_data;
                channel_count = owned_channel_count;
                frame_capacity = owned_frame_capacity;
            }

            AudioBusRole busRole() const { return bus_role; }
            void role(AudioBusRole newRole) { bus_role = newRole; }

            void bufferCapacityInFrames(uint32_t newCapacity) {
                if (newCapacity == owned_frame_capacity)
                    return;
                const uint32_t frames = newCapacity > 0 ? newCapacity : 1;
                const uint32_t channels = owned_channel_count > 0 ? owned_channel_count : 1;
                auto* newData = calloc(frames * channels, sizeof(double));
                if (owned_data && owned_frame_capacity > 0) {
                    auto toCopyFrames = std::min(owned_frame_capacity, frames);
                    if (toCopyFrames > 0)
                        std::memcpy(newData, owned_data, sizeof(double) * channels * toCopyFrames);
                    free(owned_data);
                }
                owned_data = newData;
                owned_frame_capacity = newCapacity;
                if (!aliasing) {
                    frame_capacity = newCapacity;
                    data_view = owned_data;
                }
            }

            void channelCount(uint32_t newCount) {
                if (newCount == owned_channel_count)
                    return;
                const uint32_t frames = owned_frame_capacity > 0 ? owned_frame_capacity : 1;
                const uint32_t channels = newCount > 0 ? newCount : 1;
                // Allocate (frames * channels) elements of sizeof(double) bytes each
                auto* newData = calloc(frames * channels, sizeof(double));
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

        MasterContext& master_context;
        std::vector<AudioBusBufferList*> audio_in{};
        std::vector<AudioBusBufferList*> audio_out{};
        std::vector<AudioBusSpec> audio_in_specs{};
        std::vector<AudioBusSpec> audio_out_specs{};
        EventSequence event_in;
        EventSequence event_out;
        int32_t frame_count{0};
        size_t audio_buffer_capacity_frames;
        bool replacing_enabled_{false};

    public:
        AudioProcessContext(
            MasterContext& masterContext,
            const uint32_t eventBufferSizeBytes
        ) : master_context(masterContext),
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

        void rebuildBuses(std::vector<AudioBusBufferList*>& buses,
                          std::vector<AudioBusSpec>& specsStorage,
                          const std::vector<AudioBusSpec>& requestedSpecs) {
            for (auto* bus : buses)
                delete bus;
            buses.clear();
            specsStorage = requestedSpecs;
            for (auto& spec : specsStorage) {
                size_t capacity = spec.bufferCapacityFrames;
                if (capacity == 0)
                    capacity = audio_buffer_capacity_frames > 0 ? audio_buffer_capacity_frames : 1;
                if (capacity > audio_buffer_capacity_frames)
                    audio_buffer_capacity_frames = capacity;
                auto* bus = new AudioBusBufferList(spec.channels, static_cast<uint32_t>(capacity), spec.role);
                buses.emplace_back(bus);
            }
        }

    public:
        // FIXME: there should be configure() with full list of bus configurations

        void configureAudioInputBuses(const std::vector<AudioBusSpec>& specs) {
            rebuildBuses(audio_in, audio_in_specs, specs);
        }

        void configureAudioOutputBuses(const std::vector<AudioBusSpec>& specs) {
            rebuildBuses(audio_out, audio_out_specs, specs);
        }

        void configureMainBus(int32_t inChannels, int32_t outChannels, size_t audioBufferCapacityInFrames) {
            audio_buffer_capacity_frames = audioBufferCapacityInFrames;
            std::vector<AudioBusSpec> inSpecs{{AudioBusRole::Main, static_cast<uint32_t>(inChannels), audioBufferCapacityInFrames}};
            std::vector<AudioBusSpec> outSpecs{{AudioBusRole::Main, static_cast<uint32_t>(outChannels), audioBufferCapacityInFrames}};
            configureAudioInputBuses(inSpecs);
            configureAudioOutputBuses(outSpecs);
        }

        void addAudioIn(int32_t channels, size_t audioBufferCapacityInFrames) {
            auto specs = audio_in_specs;
            specs.emplace_back(AudioBusSpec{AudioBusRole::Aux, static_cast<uint32_t>(channels), audioBufferCapacityInFrames});
            configureAudioInputBuses(specs);
        }

        void addAudioOut(int32_t channels, size_t audioBufferCapacityInFrames) {
            auto specs = audio_out_specs;
            specs.emplace_back(AudioBusSpec{AudioBusRole::Aux, static_cast<uint32_t>(channels), audioBufferCapacityInFrames});
            configureAudioOutputBuses(specs);
        }

        MasterContext& masterContext() { return master_context; }

        size_t audioBufferCapacityInFrames() { return audio_buffer_capacity_frames; }

        int32_t frameCount() const { return frame_count; }
        void frameCount(const int32_t newCount) { frame_count = newCount; }


        int32_t audioInBusCount() const { return static_cast<int32_t>(audio_in.size()); }
        int32_t audioOutBusCount() const { return static_cast<int32_t>(audio_out.size()); }

        int32_t inputChannelCount(int32_t bus) const { return bus >= audioInBusCount() ? 0 : audio_in[bus]->channelCount(); }
        int32_t outputChannelCount(int32_t bus) const { return bus >= audioOutBusCount() ? 0 : audio_out[bus]->channelCount(); }

        const std::vector<AudioBusSpec>& audioInputSpecs() const { return audio_in_specs; }
        const std::vector<AudioBusSpec>& audioOutputSpecs() const { return audio_out_specs; }
        size_t inputBusBufferCapacityInFrames(int32_t bus) const {
            return (bus < 0 || bus >= audioInBusCount()) ? 0 : audio_in[bus]->bufferCapacityInFrames();
        }
        size_t outputBusBufferCapacityInFrames(int32_t bus) const {
            return (bus < 0 || bus >= audioOutBusCount()) ? 0 : audio_out[bus]->bufferCapacityInFrames();
        }

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
            auto dataType = master_context.audioDataType();
            size_t busCount = std::min(audio_in.size(), audio_out.size());
            const size_t frames = static_cast<size_t>(std::max(frame_count, 0));
            for (size_t i = 0; i < busCount; ++i) {
                auto* inBus = audio_in[i];
                auto* outBus = audio_out[i];
                if (!inBus || !outBus)
                    continue;
                auto channels = std::min(inBus->channelCount(), outBus->channelCount());
                auto maxFrames = std::min(
                    {static_cast<size_t>(inBus->bufferCapacityInFrames()),
                     static_cast<size_t>(outBus->bufferCapacityInFrames()),
                     frames});
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    if (dataType == AudioContentType::Float64) {
                        auto* src = inBus->getDoubleBufferForChannel(ch);
                        auto* dst = outBus->getDoubleBufferForChannel(ch);
                        if (src && dst && maxFrames > 0)
                            std::memcpy(dst, src, maxFrames * sizeof(double));
                    } else {
                        auto* src = inBus->getFloatBufferForChannel(ch);
                        auto* dst = outBus->getFloatBufferForChannel(ch);
                        if (src && dst && maxFrames > 0)
                            std::memcpy(dst, src, maxFrames * sizeof(float));
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
            auto dataType = master_context.audioDataType();
            const size_t frames = static_cast<size_t>(std::max(frame_count, 0));
            // Copy audio output to input for the next node
            for (size_t i = 0; i < std::min(audio_in.size(), audio_out.size()); ++i) {
                auto* inBus = audio_in[i];
                auto* outBus = audio_out[i];
                if (inBus && outBus) {
                    auto channels = std::min(inBus->channelCount(), outBus->channelCount());
                    auto maxFrames = std::min(
                        {static_cast<size_t>(inBus->bufferCapacityInFrames()),
                         static_cast<size_t>(outBus->bufferCapacityInFrames()),
                         frames});
                    for (uint32_t ch = 0; ch < channels; ++ch) {
                        if (dataType == AudioContentType::Float64) {
                            auto* dst = inBus->getDoubleBufferForChannel(ch);
                            auto* src = outBus->getDoubleBufferForChannel(ch);
                            if (dst && src && maxFrames > 0)
                                std::memcpy(dst, src, maxFrames * sizeof(double));
                        } else {
                            auto* dst = inBus->getFloatBufferForChannel(ch);
                            auto* src = outBus->getFloatBufferForChannel(ch);
                            if (dst && src && maxFrames > 0)
                                std::memcpy(dst, src, maxFrames * sizeof(float));
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
