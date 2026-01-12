#include "DefaultDeviceIODispatcher.hpp"
#include "uapmd/uapmd.hpp"
#include <cstring>

namespace uapmd {

    DefaultDeviceIODispatcher::DefaultDeviceIODispatcher() {}

    DefaultDeviceIODispatcher::~DefaultDeviceIODispatcher() {
        if (audio_)
            audio_->clearAudioCallbacks();
        callbacks.clear();
        if (midi_in && midi_input_handler_registered) {
            midi_in->removeInputHandler(midiInputTrampoline);
            midi_input_handler_registered = false;
        }
        if (queued_inputs) {
            free(queued_inputs);
            queued_inputs = nullptr;
        }
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

        return audio_->start();
    }

    uapmd_status_t DefaultDeviceIODispatcher::stop() {
        if (!audio_)
            return 0;

        audio_thread_id.reset();

        return audio_->stop();
    }

    bool DefaultDeviceIODispatcher::isPlaying() {
        return audio_ && audio_->isPlaying();
    }

    uapmd_status_t DefaultDeviceIODispatcher::configure(size_t umpBufferSizeInBytes, AudioIODevice *audio,
                                                        MidiIODevice *midiIn, MidiIODevice *midiOut) {
        ump_buffer_size_in_bytes = umpBufferSizeInBytes;
        if (queued_inputs) {
            free(queued_inputs);
            queued_inputs = nullptr;
        }
        queued_inputs = static_cast<uapmd_ump_t*>(calloc(1, ump_buffer_size_in_bytes));
        queued_input_bytes = 0;

        if (audio_)
            audio_->clearAudioCallbacks();
        if (midi_in && midi_input_handler_registered) {
            midi_in->removeInputHandler(midiInputTrampoline);
            midi_input_handler_registered = false;
        }

        audio_ = audio;
        midi_in = midiIn;
        midi_out = midiOut;
        if (audio_)
            audio_->addAudioCallback([this](auto& data) {
                return runCallbacks(data);
            });
        if (midi_in) {
            midi_in->addInputHandler(midiInputTrampoline, this);
            midi_input_handler_registered = true;
        }

        return (uapmd_status_t) 0;
    }

    uapmd_status_t DefaultDeviceIODispatcher::runCallbacks(AudioProcessContext& data) {
        drainQueuedMidi(data);
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

    void DefaultDeviceIODispatcher::drainQueuedMidi(AudioProcessContext& data) {
        if (!queued_inputs || queued_input_bytes == 0)
            return;
        std::lock_guard<std::mutex> lock(midi_mutex);
        auto& eventIn = data.eventIn();
        auto capacity = eventIn.maxMessagesInBytes();
        auto bytes_to_copy = std::min(capacity, queued_input_bytes);
        if (bytes_to_copy > 0) {
            std::memcpy(eventIn.getMessages(), queued_inputs, bytes_to_copy);
            eventIn.position(bytes_to_copy);
        }
        queued_input_bytes = 0;
    }

    void DefaultDeviceIODispatcher::midiInputTrampoline(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        (void) timestamp;
        if (!context || !ump || sizeInBytes == 0)
            return;
        static_cast<DefaultDeviceIODispatcher*>(context)->enqueueMidiInput(ump, sizeInBytes);
    }

    void DefaultDeviceIODispatcher::enqueueMidiInput(uapmd_ump_t* ump, size_t sizeInBytes) {
        if (!queued_inputs || sizeInBytes == 0)
            return;
        std::lock_guard<std::mutex> lock(midi_mutex);
        if (queued_input_bytes + sizeInBytes > ump_buffer_size_in_bytes)
            return;
        std::memcpy(reinterpret_cast<uint8_t*>(queued_inputs) + queued_input_bytes, ump, sizeInBytes);
        queued_input_bytes += sizeInBytes;
    }
}
