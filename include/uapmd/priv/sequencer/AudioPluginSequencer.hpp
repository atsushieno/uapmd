#pragma once

#include "remidy-tooling/PluginInstancing.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginSequencer {
        const size_t buffer_size_in_frames;
        const size_t ump_buffer_size_in_bytes;
        int32_t sample_rate;

        DeviceIODispatcher dispatcher{};
        AudioPluginHostPAL* plugin_host_pal;
        SequenceProcessor sequencer;

    public:
        AudioPluginSequencer(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate);

        // Audio plugin support

        void performPluginScanning(bool rescan);

        PluginCatalog& catalog();

        void instantiatePlugin(std::string& format, std::string& pluginId,
            std::function<void(int32_t instanceId, std::string error)> callback);

        std::vector<ParameterMetadata> getParameterList(int32_t instanceId);
        std::vector<PresetsMetadata> getPresetList(int32_t instanceId);
        void loadPreset(int32_t instanceId, int32_t presetIndex);

        // audio/MIDI player

        void sendNoteOn(int32_t trackIndex, int32_t note);
        void sendNoteOff(int32_t trackIndex, int32_t note);
        void setParameterValue(int32_t instanceId, int32_t index, double value);
        void enqueueUmp(int32_t trackIndex, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);

        // Audio controller (WIP, unused yet)

        uapmd_status_t startAudio();
        uapmd_status_t stopAudio();
        uapmd_status_t isAudioPlaying();

        int32_t sampleRate();
        bool sampleRate(int32_t newSampleRate);

        std::vector<uint8_t> saveState();
        void loadState(std::vector<uint8_t>& state);
    };
}

