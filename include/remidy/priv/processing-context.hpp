#pragma once

#include "../remidy.hpp"

namespace remidy {
    // Represents a list of audio buffers in an audio bus, separate per channel.
    // It is part of `AudioProcessingContext`.
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

        float* getFloatBufferForChannel(uint32_t channel) const { return channel >= channel_count ? nullptr : static_cast<float *>(data) + channel * frame_capacity; }
        double* getDoubleBufferForChannel(uint32_t channel) const { return channel >= channel_count ? nullptr : static_cast<double *>(data) + channel * frame_capacity; };
        uint32_t channelCount() const { return channel_count; }
        uint32_t bufferCapacityInFrames() const { return frame_capacity; }
    };

    // Represents a sample-accurate sequence of UMPs.
    // It is part of `AudioProcessingContext`.
    class MidiSequence {
        std::vector<remidy_ump_t> messages{};
    public:
        explicit MidiSequence(size_t messageBufferSizeInInt) : messages(messageBufferSizeInInt) {}
        remidy_ump_t* getMessages() { return messages.data(); }
        size_t sizeInInts() const { return messages.size(); }
        size_t sizeInBytes() const { return messages.size() * sizeof(remidy_ump_t); }
    };

    class AudioProcessContext {
        std::vector<AudioBusBufferList*> audio_in{};
        std::vector<AudioBusBufferList*> audio_out{};
        MidiSequence midi_in;
        MidiSequence midi_out;
        int32_t frame_count{0};

    public:
        AudioProcessContext(
            const uint32_t umpBufferSizeInInts
        ) : midi_in(umpBufferSizeInInts),
            midi_out(umpBufferSizeInInts) {
        }
        ~AudioProcessContext() {
            for (const auto bus : audio_in)
                if (bus)
                    delete bus;
            for (const auto bus : audio_out)
                if (bus)
                    delete bus;
        }

        int32_t frameCount() const { return frame_count; }
        void frameCount(const int32_t newCount) { frame_count = newCount; }


        int32_t audioInBusCount() { return audio_in.size(); }
        int32_t audioOutBusCount() { return audio_out.size(); }
        AudioBusBufferList* audioIn(int32_t bus) const { return audio_in[bus]; }
        AudioBusBufferList* audioOut(int32_t bus) const { return audio_out[bus]; }

        void addAudioIn(uint32_t channelCount, uint32_t bufferSizeInFrames) {
            audio_in.emplace_back(new AudioBusBufferList(channelCount, bufferSizeInFrames));
        }
        void addAudioOut(uint32_t channelCount, uint32_t bufferSizeInFrames) {
            audio_out.emplace_back(new AudioBusBufferList(channelCount, bufferSizeInFrames));
        }

        MidiSequence& midiIn() { return midi_in; }
        MidiSequence& midiOut() { return midi_out; }
    };

}