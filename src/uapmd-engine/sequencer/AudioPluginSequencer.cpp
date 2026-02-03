
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <utility>
#include "uapmd-engine/uapmd-engine.hpp"

// Note: audio file decoding is abstracted behind uapmd::AudioFileReader interface now.


uapmd::AudioPluginSequencer::AudioPluginSequencer(
    size_t audioBufferSizeInFrames,
    size_t umpBufferSizeInBytes,
    int32_t sampleRate,
    DeviceIODispatcher* dispatcher
) : buffer_size_in_frames(audioBufferSizeInFrames),
    ump_buffer_size_in_bytes(umpBufferSizeInBytes), sample_rate(sampleRate),
    dispatcher(dispatcher),
    sequencer(SequencerEngine::create(sampleRate, buffer_size_in_frames, umpBufferSizeInBytes)) {
    // Configure default channels based on audio device
    auto audioDevice = dispatcher->audio();
    const auto inputChannels = audioDevice ? std::max(audioDevice->inputChannels(), 2u) : 2;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 2;
    sequencer->setDefaultChannels(inputChannels, outputChannels);

    auto manager = AudioIODeviceManager::instance();
    auto logger = remidy::Logger::global();
    AudioIODeviceManager::Configuration audioConfig{ .logger = logger };
    manager->initialize(audioConfig);

    // FIXME: enable MIDI devices
    dispatcher->configure(umpBufferSizeInBytes, manager->open());

    dispatcher->addCallback([&](uapmd::AudioProcessContext& process) {
        // Delegate all master audio processing to SequencerEngine
        return sequencer->processAudio(process);
    });
}

uapmd::AudioPluginSequencer::~AudioPluginSequencer() {
    stopAudio();
}

std::string uapmd::AudioPluginSequencer::getPluginFormat(int32_t instanceId) {
    auto* instance = sequencer->getPluginInstance(instanceId);
    return instance->formatName();
}

int32_t uapmd::AudioPluginSequencer::findTrackIndexForInstance(int32_t instanceId) const {
    auto& tracksRef = sequencer->tracks();
    for (size_t i = 0; i < tracksRef.size(); ++i) {
        for (auto& p : tracksRef[i]->graph().plugins()) {
            if (p.first == instanceId) {
                return static_cast<int32_t>(i);
            }
        }
    }
    return -1;
}

uapmd_status_t uapmd::AudioPluginSequencer::startAudio() {
    return dispatcher->start();
}

uapmd_status_t uapmd::AudioPluginSequencer::stopAudio() {
    return dispatcher->stop();
}

uapmd_status_t uapmd::AudioPluginSequencer::isAudioPlaying() {
    return dispatcher->isPlaying();
}

int32_t uapmd::AudioPluginSequencer::sampleRate() { return sample_rate; }
bool uapmd::AudioPluginSequencer::sampleRate(int32_t newSampleRate) {
    if (dispatcher->isPlaying())
        return false;
    sample_rate = newSampleRate;
    return true;
}

bool uapmd::AudioPluginSequencer::reconfigureAudioDevice(int inputDeviceIndex, int outputDeviceIndex, uint32_t sampleRate, uint32_t bufferSize) {
    // Stop audio if it's currently playing
    bool wasPlaying = dispatcher->isPlaying();
    if (wasPlaying) {
        if (stopAudio() != 0) {
            remidy::Logger::global()->logError("Failed to stop audio during device reconfiguration");
            return false;
        }
    }

    // Get a new audio device from the manager with specific device indices
    auto manager = AudioIODeviceManager::instance();
    auto* newDevice = manager->open(inputDeviceIndex, outputDeviceIndex, sampleRate);
    if (!newDevice) {
        remidy::Logger::global()->logError("Failed to open audio device with indices: input={}, output={}, sampleRate={}", inputDeviceIndex, outputDeviceIndex, sampleRate);
        return false;
    }

    // Update the sample rate if specified
    if (sampleRate > 0) {
        sample_rate = sampleRate;
    } else {
        // Use the device's actual sample rate
        sample_rate = static_cast<int32_t>(newDevice->sampleRate());
    }

    // Update the buffer size if specified
    if (bufferSize > 0) {
        buffer_size_in_frames = bufferSize;
        // Reconfigure all track contexts with the new buffer size
        auto& tracks = sequencer->tracks();
        auto& data = sequencer->data();
        for (size_t i = 0; i < tracks.size() && i < data.tracks.size(); i++) {
            auto* trackCtx = data.tracks[i];
            if (trackCtx) {
                // Get channel counts from the track's current configuration
                uint32_t inputChannels = trackCtx->audioInBusCount() > 0 ? trackCtx->inputChannelCount(0) : 2;
                uint32_t outputChannels = trackCtx->audioOutBusCount() > 0 ? trackCtx->outputChannelCount(0) : 2;

                // Reconfigure the track context with new buffer size
                // Note: This will recreate the audio buffers with the new size
                trackCtx->configureMainBus(inputChannels, outputChannels, buffer_size_in_frames);
            }
        }
    }

    // Reconfigure the dispatcher with the new device
    if (dispatcher->configure(ump_buffer_size_in_bytes, newDevice) != 0) {
        remidy::Logger::global()->logError("Failed to reconfigure dispatcher with new audio device");
        return false;
    }

    // Update SequencerEngine with new channel counts
    auto audioDevice = dispatcher->audio();
    const auto inputChannels = audioDevice ? std::max(audioDevice->inputChannels(), 2u) : 2;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 2;
    sequencer->setDefaultChannels(inputChannels, outputChannels);

    // Restart audio if it was playing before
    if (wasPlaying) {
        if (startAudio() != 0) {
            remidy::Logger::global()->logError("Failed to restart audio after device reconfiguration");
            return false;
        }
    }

    return true;
}
