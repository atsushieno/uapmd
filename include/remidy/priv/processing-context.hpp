#pragma once
#if defined(_MSC_VER) // wow, that's stupid... https://stackoverflow.com/questions/5004858/why-is-stdmin-failing-when-windows-h-is-included
#define NOMINMAX
#endif

#include <algorithm>
#include <cstring>
#include <vector>

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
            // FIXME: calculate
            return 0;
        }
    };

    // Represents a set of realtime-safe input and output of audio processing.
    // Anything that is NOT RT-safe manipulation has to be done outside of this structure.
    class AudioProcessContext {

        // FIXME: remove this class and manage the entire audio buffers in one single array.
        class AudioBusBufferList {
            uint32_t channel_count{};
            uint32_t frame_capacity{};
            void* data{nullptr};

        public:
            AudioBusBufferList(uint32_t channelCount, uint32_t bufferSizeInFrames) :
                    channel_count(channelCount),
                    frame_capacity(bufferSizeInFrames) {
                data = calloc(sizeof(double) * bufferCapacityInFrames(), channel_count);
            }
            ~AudioBusBufferList() {
                free(data);
            }

            void clear() {
                if (!data)
                    return;
                std::memset(data, 0, channel_count * frame_capacity * sizeof(double));
            }

            float* getFloatBufferForChannel(uint32_t channel) const { return channel >= channel_count ? nullptr : static_cast<float *>(data) + channel * frame_capacity; }
            double* getDoubleBufferForChannel(uint32_t channel) const { return channel >= channel_count ? nullptr : static_cast<double *>(data) + channel * frame_capacity; };
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


        int32_t audioInBusCount() { return audio_in.size(); }
        int32_t audioOutBusCount() { return audio_out.size(); }

        int32_t inputChannelCount(int32_t bus) { return audio_in[bus]->channelCount(); }
        int32_t outputChannelCount(int32_t bus) { return audio_out[bus]->channelCount(); }

        float* getFloatInBuffer(int32_t bus, uint32_t channel) const {
            auto b = audio_in[bus];
            return channel >= b->channelCount() ? nullptr : b->getFloatBufferForChannel(channel);
        }
        float* getFloatOutBuffer(int32_t bus, uint32_t channel) const {
            auto b = audio_out[bus];
            return channel >= b->channelCount() ? nullptr : b->getFloatBufferForChannel(channel);
        }
        double* getDoubleInBuffer(int32_t bus, uint32_t channel) const {
            auto b = audio_in[bus];
            return channel >= b->channelCount() ? nullptr : b->getDoubleBufferForChannel(channel);
        };
        double* getDoubleOutBuffer(int32_t bus, uint32_t channel) const {
            auto b = audio_out[bus];
            return channel >= b->channelCount() ? nullptr : b->getDoubleBufferForChannel(channel);
        };

        EventSequence& eventIn() { return event_in; }
        EventSequence& eventOut() { return event_out; }

        void clearAudioOutputs() {
            for (auto* bus : audio_out)
                if (bus)
                    bus->clear();
            event_out.position(0);
        }

        void advanceToNextNode() {
            // Copy audio output to input for the next node
            for (size_t i = 0; i < std::min(audio_in.size(), audio_out.size()); ++i) {
                auto* inBus = audio_in[i];
                auto* outBus = audio_out[i];
                if (inBus && outBus) {
                    auto channels = std::min(inBus->channelCount(), outBus->channelCount());
                    auto frames = std::min(inBus->bufferCapacityInFrames(), outBus->bufferCapacityInFrames());
                    for (uint32_t ch = 0; ch < channels; ++ch) {
                        // Copy based on the audio data type
                        std::memcpy(inBus->getFloatBufferForChannel(ch),
                                   outBus->getFloatBufferForChannel(ch),
                                   frames * sizeof(float));
                    }
                }
            }

            // Clear output buffers for the next plugin
            for (auto* bus : audio_out)
                if (bus)
                    bus->clear();

            // Copy events output to input for the next node
            auto eventBytes = std::min(event_in.maxMessagesInBytes(), event_out.position());
            if (eventBytes > 0) {
                std::memcpy(event_in.getMessages(), event_out.getMessages(), eventBytes);
                event_in.position(eventBytes);
            }
            event_out.position(0);
        }
    };

}
