#include "uapmd/uapmd.hpp"


namespace uapmd {
    class DeviceIODispatcher::Impl {
        std::vector<std::function<uapmd_status_t(AudioProcessContext& data)>> callbacks{};
        DeviceIODispatcher* owner{};
        AudioIODevice* audio{};
        MidiIODevice* midi{};
        uapmd_ump_t* queued_inputs{};
        std::atomic<size_t> next_ump_position{0};
        size_t ump_buffer_size_in_bytes{0};

    public:
        explicit Impl(size_t umpInputBufferSizeInBytes, DeviceIODispatcher* owner, AudioIODevice* audioDriver, MidiIODevice* midiDriver);

        ~Impl() {
            audio->clearAudioCallbacks();
            callbacks.clear();
            free(queued_inputs);
        }

        AudioIODevice* audioDriver() { return audio; }
        MidiIODevice* midiDriver() { return midi; }

        void addCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) {
            callbacks.emplace_back(std::move(callback));
        }
        uapmd_status_t start();
        uapmd_status_t stop();
        bool isPlaying();

        uapmd_status_t runCallbacks(AudioProcessContext& data) {
            // FIXME: define status codes
            for (auto& callback : callbacks)
                if (auto status = callback(data); status != 0)
                    return status;
            return 0;
        }
    };
}

uapmd::DeviceIODispatcher::DeviceIODispatcher(size_t umpBufferSizeInBytes, AudioIODevice* audioDriver, MidiIODevice* midiDriver) :
    impl(new Impl(umpBufferSizeInBytes, this, audioDriver ? audioDriver : AudioIODevice::instance(), midiDriver ? midiDriver : MidiIODevice::instance())) {
}

uapmd::DeviceIODispatcher::~DeviceIODispatcher() {
    delete impl;
}

uapmd::AudioIODevice* uapmd::DeviceIODispatcher::audioDriver() { return impl->audioDriver(); }
uapmd::MidiIODevice* uapmd::DeviceIODispatcher::midiDriver() { return impl->midiDriver(); }

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

// Impl

uapmd::DeviceIODispatcher::Impl::Impl(size_t umpInputBufferSizeInBytes, DeviceIODispatcher* owner, AudioIODevice* audioDriver, MidiIODevice* midiDriver) :
    owner(owner), audio(audioDriver), midi(midiDriver), ump_buffer_size_in_bytes(umpInputBufferSizeInBytes) {
    queued_inputs = (uapmd_ump_t*) calloc(1, umpInputBufferSizeInBytes);
    audioDriver->addAudioCallback([this](auto& data) {
        auto ret = runCallbacks(data);
        next_ump_position = 0; // clear MIDI input queue
        return ret;
    });
    midiDriver->addCallback([this](AudioProcessContext& data) {
        auto& input = data.eventIn();
        size_t size = input.position();
        if (size + next_ump_position >= ump_buffer_size_in_bytes)
            return 1; // FIXME: define error code for insufficient buffer
        // FIXME: define status codes
        memcpy(queued_inputs + next_ump_position, input.getMessages(), size);
        next_ump_position += size;
        return 0;
    });
}


uapmd_status_t uapmd::DeviceIODispatcher::Impl::start() {
    return audio->start() || midi->start();
}

uapmd_status_t uapmd::DeviceIODispatcher::Impl::stop() {
    return audio->stop() || midi->start();
}

bool uapmd::DeviceIODispatcher::Impl::isPlaying() {
    return audio->isPlaying();
}
