#pragma once

#include <functional>
#include <string>
#include <vector>

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
        struct PluginNodeInfo {
            int32_t instanceId = -1;
            std::string pluginId;
            std::string format;
            std::string displayName;
        };

        struct TrackInfo {
            int32_t trackIndex = -1;
            std::vector<PluginNodeInfo> nodes;
        };

        AudioPluginSequencer(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate);
        ~AudioPluginSequencer();

        // Audio plugin support

        void performPluginScanning(bool rescan);

        PluginCatalog& catalog();

        void instantiatePlugin(std::string& format, std::string& pluginId,
            std::function<void(int32_t instanceId, std::string error)> callback);
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId,
            std::function<void(int32_t instanceId, std::string error)> callback);
        bool removePluginInstance(int32_t instanceId);

        std::vector<ParameterMetadata> getParameterList(int32_t instanceId);
        std::vector<PresetsMetadata> getPresetList(int32_t instanceId);
        void loadPreset(int32_t instanceId, int32_t presetIndex);
        std::vector<int32_t> getInstanceIds();
        std::string getPluginName(int32_t instanceId);
        std::string getPluginFormat(int32_t instanceId);
        std::vector<TrackInfo> getTrackInfos();
        int32_t findTrackIndexForInstance(int32_t instanceId) const;

        // We will have to split out these GUI features at some point...
        bool hasPluginUI(int32_t instanceId);
        bool showPluginUI(int32_t instanceId, bool isFloating, void* parentHandle);
        bool createPluginUI(int32_t instanceId, bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler);
        void destroyPluginUI(int32_t instanceId);
        void hidePluginUI(int32_t instanceId);
        bool isPluginUIVisible(int32_t instanceId);
        bool resizePluginUI(int32_t instanceId, uint32_t width, uint32_t height);
        bool getPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height);
        bool canPluginUIResize(int32_t instanceId);

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
