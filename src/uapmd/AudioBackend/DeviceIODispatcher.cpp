#include "uapmd/uapmd.hpp"
#include <cstring>

namespace uapmd {
    class DeviceIODispatcher::Impl {
        std::vector<std::function<uapmd_status_t(AudioProcessContext& data)>> callbacks{};
        DeviceIODispatcher* owner{};
        AudioIODevice* audio_{};
        MidiIODevice* midi_in{};
        MidiIODevice* midi_out{};
        uapmd_ump_t* queued_inputs{};
        std::atomic<size_t> next_ump_position{0};
        size_t ump_buffer_size_in_bytes{0};
        std::optional<std::thread::id> audio_thread_id{};

    public:
        explicit Impl(DeviceIODispatcher* owner);
        uapmd_status_t configure(size_t umpInputBufferSizeInBytes, AudioIODevice* audio, MidiIODevice* midiIn, MidiIODevice* midiOut);

        ~Impl() {
            if (audio_)
                audio_->clearAudioCallbacks();
            callbacks.clear();
            if (queued_inputs)
                free(queued_inputs);
        }

        AudioIODevice* audio() { return audio_; }
        MidiIODevice* midiIn() { return midi_in; }
        MidiIODevice* midiOut() { return midi_out; }

        void addCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) {
            callbacks.emplace_back(std::move(callback));
        }
        uapmd_status_t start();
        uapmd_status_t stop();
        bool isPlaying();

        uapmd_status_t runCallbacks(AudioProcessContext& data) {
            if (!audio_thread_id.has_value()) {
                audio_thread_id = std::this_thread::get_id();
                remidy::audioThreadIds().push_back(audio_thread_id.value());
            }
            // FIXME: define status codes
            for (auto& callback : callbacks)
                if (auto status = callback(data); status != 0)
                    return status;
            return 0;
        }
    };
}

uapmd::DeviceIODispatcher::DeviceIODispatcher() :
    impl(new Impl(this)) {
}

uapmd::DeviceIODispatcher::~DeviceIODispatcher() {
    delete impl;
}

uapmd::AudioIODevice* uapmd::DeviceIODispatcher::audio() { return impl->audio(); }
uapmd::MidiIODevice* uapmd::DeviceIODispatcher::midiIn() { return impl->midiIn(); }
uapmd::MidiIODevice* uapmd::DeviceIODispatcher::midiOut() { return impl->midiOut(); }

void uapmd::DeviceIODispatcher::addCallback(std::function<uapmd_status_t(AudioProcessContext &)> &&callback) {
    impl->addCallback(std::move(callback));
}

uapmd_status_t uapmd::DeviceIODispatcher::start() {
    return impl->start();
}

uapmd_status_t uapmd::DeviceIODispatcher::stop() {
    return impl->stop();
}

bool uapmd::DeviceIODispatcher::isPlaying() {
    return impl->isPlaying();
}

uapmd_status_t uapmd::DeviceIODispatcher::manuallyRunCallbacks(uapmd::AudioProcessContext &data) {
    return impl->runCallbacks(data);
}

uapmd_status_t uapmd::DeviceIODispatcher::configure(size_t umpBufferSizeInBytes, uapmd::AudioIODevice *audio,
                                                    uapmd::MidiIODevice *midiIn, uapmd::MidiIODevice *midiOut) {
    return impl->configure(umpBufferSizeInBytes, audio, midiIn, midiOut);
}

// Impl

uapmd::DeviceIODispatcher::Impl::Impl(DeviceIODispatcher* owner) : owner(owner) {}

uapmd_status_t uapmd::DeviceIODispatcher::Impl::configure(
        size_t umpInputBufferSizeInBytes,
        AudioIODevice* audio,
        MidiIODevice* midiIn,
        MidiIODevice* midiOut) {
    ump_buffer_size_in_bytes = umpInputBufferSizeInBytes;
    audio_ = audio;
    midi_in = midiIn;
    midi_out = midiOut;
    queued_inputs = (uapmd_ump_t*) calloc(1, umpInputBufferSizeInBytes);
    if (audio)
        audio->addAudioCallback([this](auto& data) {
            auto ret = runCallbacks(data);
            next_ump_position = 0; // clear MIDI input queue
            return ret;
        });
    if (midi_in)
        midi_in->addCallback([this](AudioProcessContext& data) {
            auto& input = data.eventIn();
            size_t size = input.position();
            if (size + next_ump_position >= ump_buffer_size_in_bytes)
                return 1; // FIXME: define error code for insufficient buffer
            memcpy(queued_inputs + next_ump_position, input.getMessages(), size);
            next_ump_position += size;
            // FIXME: define status codes
            return 0;
        });

    // FIXME: define status codes
    return (uapmd_status_t) 0;
}


uapmd_status_t uapmd::DeviceIODispatcher::Impl::start() {
    // FIXME: define status codes (0 == success)
    if (!audio_)
        return -1;

    return audio_->start()
        || (midi_in ? midi_in->start() : 0)
        || (midi_out ? midi_out->start() : 0)
        ;
}

uapmd_status_t uapmd::DeviceIODispatcher::Impl::stop() {
    // FIXME: define status codes (0 == success)
    if (!audio_)
        return 0; // ok; stop at uninitialized state

    audio_thread_id.reset();

    auto ret = audio_->stop();
    ret |= (midi_in ? midi_in->stop() : 0);
    ret |= (midi_out ? midi_out->stop() : 0);
    return ret;
}

bool uapmd::DeviceIODispatcher::Impl::isPlaying() {
    return audio_ && audio_->isPlaying();
}
