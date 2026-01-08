#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "remidy/remidy.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginSequencer {
        size_t buffer_size_in_frames;
        const size_t ump_buffer_size_in_bytes;
        int32_t sample_rate;

        DeviceIODispatcher* dispatcher{};
        std::unique_ptr<SequencerEngine> sequencer;

        // Offline rendering mode
        std::atomic<bool> offline_rendering_{false};

        using PluginOutputHandler = std::function<void(const uapmd_ump_t*, size_t)>;
        using HandlerMap = std::unordered_map<int32_t, PluginOutputHandler>;
        std::shared_ptr<HandlerMap> plugin_output_handlers_;

        AudioPluginNode* findPluginNodeByInstance(int32_t instanceId);

    public:
        AudioPluginSequencer(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate, DeviceIODispatcher* dispatcher);
        ~AudioPluginSequencer();

        SequencerEngine* engine() const { return sequencer.get(); }

        // Audio plugin support

        // Application-specific plugin management wrappers
        bool removePluginInstance(int32_t instanceId);

        // Application-specific queries (metadata, track info)
        std::vector<int32_t> getInstanceIds();
        std::string getPluginFormat(int32_t instanceId);
        int32_t findTrackIndexForInstance(int32_t instanceId) const;

        // Application-specific MIDI device integration
        void setPluginOutputHandler(int32_t instanceId, PluginOutputHandler handler);
        void assignMidiDeviceToPlugin(int32_t instanceId, std::shared_ptr<MidiIODevice> device);
        void clearMidiDeviceFromPlugin(int32_t instanceId);

        // Audio controller

        uapmd_status_t startAudio();
        uapmd_status_t stopAudio();
        uapmd_status_t isAudioPlaying();

        int64_t playbackPositionSamples() const;

        int32_t sampleRate();
        bool sampleRate(int32_t newSampleRate);

        // Reconfigure audio device (stops audio, reconfigures, restarts)
        // Use -1 for default device, 0 for no sample rate change, 0 for no buffer size change
        bool reconfigureAudioDevice(int inputDeviceIndex = -1, int outputDeviceIndex = -1, uint32_t sampleRate = 0, uint32_t bufferSize = 0);

        bool offlineRendering() const;
        void offlineRendering(bool enabled);
    };
}
