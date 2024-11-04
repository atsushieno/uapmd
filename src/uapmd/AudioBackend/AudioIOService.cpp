#include "uapmd/uapmd.hpp"


namespace uapmd {
    class AudioIOService::Impl {
        AudioIOService* owner;
        AudioIODriver* driver;
    public:
        explicit Impl(AudioIOService* owner, AudioIODriver* driver) : owner(owner), driver(driver) {
        }

        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback);
        uapmd_status_t start();
        uapmd_status_t stop();
    };
}

uapmd::AudioIOService::AudioIOService(AudioIODriver* driver) : impl(new Impl(this, driver = driver ? driver : AudioIODriver::instance())) {
}

uapmd::AudioIOService::~AudioIOService() {
    delete impl;
}

void uapmd::AudioIOService::addAudioCallback(std::function<uapmd_status_t(AudioProcessContext &)> &&callback) {
    impl->addAudioCallback(std::move(callback));
}

uapmd_status_t uapmd::AudioIOService::start() {
    return impl->start();
}

uapmd_status_t uapmd::AudioIOService::stop() {
    return impl->stop();
}

// Impl

void uapmd::AudioIOService::Impl::addAudioCallback(std::function<uapmd_status_t(AudioProcessContext &)> &&callback) {

}

uapmd_status_t uapmd::AudioIOService::Impl::start() {
    return driver->start();
}

uapmd_status_t uapmd::AudioIOService::Impl::stop() {
    return driver->stop();
}
