#include "DefaultDeviceIODispatcher.hpp"
#include "uapmd/uapmd.hpp"
#include <cstring>

namespace uapmd {

    DefaultDeviceIODispatcher::DefaultDeviceIODispatcher() {}

    DefaultDeviceIODispatcher::~DefaultDeviceIODispatcher() {
        if (audio_)
            audio_->clearAudioCallbacks();
        callbacks.clear();
        if (queued_inputs)
            free(queued_inputs);
    }

    AudioIODevice* DefaultDeviceIODispatcher::audio() { return audio_; }
    MidiIODevice* DefaultDeviceIODispatcher::midiIn() { return midi_in; }
    MidiIODevice* DefaultDeviceIODispatcher::midiOut() { return midi_out; }

    void DefaultDeviceIODispatcher::addCallback(std::function<uapmd_status_t(AudioProcessContext &)> &&callback) {
        callbacks.emplace_back(std::move(callback));
    }

    uapmd_status_t DefaultDeviceIODispatcher::start() {
        if (!audio_)
            return -1;

        return audio_->start()
            || (midi_in ? midi_in->start() : 0)
            || (midi_out ? midi_out->start() : 0)
            ;
    }

    uapmd_status_t DefaultDeviceIODispatcher::stop() {
        if (!audio_)
            return 0;

        audio_thread_id.reset();

        auto ret = audio_->stop();
        ret |= (midi_in ? midi_in->stop() : 0);
        ret |= (midi_out ? midi_out->stop() : 0);
        return ret;
    }

    bool DefaultDeviceIODispatcher::isPlaying() {
        return audio_ && audio_->isPlaying();
    }

    uapmd_status_t DefaultDeviceIODispatcher::configure(size_t umpBufferSizeInBytes, AudioIODevice *audio,
                                                        MidiIODevice *midiIn, MidiIODevice *midiOut) {
        ump_buffer_size_in_bytes = umpBufferSizeInBytes;
        audio_ = audio;
        midi_in = midiIn;
        midi_out = midiOut;
        queued_inputs = (uapmd_ump_t*) calloc(1, umpBufferSizeInBytes);
        if (audio)
            audio->addAudioCallback([this](auto& data) {
                auto ret = runCallbacks(data);
                next_ump_position = 0;
                return ret;
            });
        if (midi_in)
            midi_in->addCallback([this](AudioProcessContext& data) {
                auto& input = data.eventIn();
                size_t size = input.position();
                if (size + next_ump_position >= ump_buffer_size_in_bytes)
                    return 1;
                memcpy(queued_inputs + next_ump_position, input.getMessages(), size);
                next_ump_position += size;
                return 0;
            });

        return (uapmd_status_t) 0;
    }

    uapmd_status_t DefaultDeviceIODispatcher::runCallbacks(AudioProcessContext& data) {
        if (!audio_thread_id.has_value()) {
            audio_thread_id = std::this_thread::get_id();
            remidy::audioThreadIds().push_back(audio_thread_id.value());
        }
        for (auto& callback : callbacks)
            if (auto status = callback(data); status != 0)
                return status;
        return 0;
    }

    std::unique_ptr<DefaultDeviceIODispatcher> default_dispatcher{};

    DeviceIODispatcher* defaultDeviceIODispatcher() {
        if (!default_dispatcher)
            default_dispatcher = std::make_unique<DefaultDeviceIODispatcher>();
        return default_dispatcher.get();
    }
}
