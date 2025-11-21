#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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

        // Playback control
        std::atomic<bool> is_playback_active_{false};
        std::atomic<int64_t> playback_position_samples_{0};
        std::atomic<bool> offline_rendering_{false};

        struct FunctionBlockRoute {
            AudioPluginTrack* track{nullptr};
            int32_t trackIndex{-1};
        };

        std::unordered_map<int32_t, FunctionBlockRoute> plugin_function_blocks_;
        mutable std::unordered_map<int32_t, uint8_t> plugin_groups_;
        mutable std::unordered_map<uint8_t, int32_t> group_to_instance_;
        std::vector<uint8_t> free_groups_;
        uint8_t next_group_{0};

        using PluginOutputHandler = std::function<void(const uapmd_ump_t*, size_t)>;
        using HandlerMap = std::unordered_map<int32_t, PluginOutputHandler>;
        std::shared_ptr<HandlerMap> plugin_output_handlers_;
        std::vector<uapmd_ump_t> plugin_output_scratch_;

        struct ParameterUpdate {
            int32_t parameterIndex;
            double value;
        };
        std::unordered_map<int32_t, std::vector<ParameterUpdate>> pending_parameter_updates_;

        // Plugin instance management
        std::unordered_map<int32_t, AudioPluginHostPAL::AudioPluginNodePAL*> plugin_instances_;
        std::unordered_map<int32_t, bool> plugin_bypassed_;
        mutable std::mutex instance_map_mutex_;

        struct RouteResolution {
            AudioPluginTrack* track{nullptr};
            int32_t trackIndex{-1};
            int32_t instanceId{-1};
        };

        std::optional<RouteResolution> resolveTarget(int32_t trackOrInstanceId);
        void refreshFunctionBlockMappings();
        void configureTrackRouting(AudioPluginTrack* track);
        void dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes);
        uint8_t assignGroup(int32_t instanceId);
        void releaseGroup(int32_t instanceId);
        std::optional<uint8_t> groupForInstanceOptional(int32_t instanceId) const;
        std::optional<int32_t> instanceForGroupOptional(uint8_t group) const;

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

        void addSimplePluginTrack(std::string& format, std::string& pluginId,
                                  std::function<void(int32_t instanceId, std::string error)> callback);
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId,
            std::function<void(int32_t instanceId, std::string error)> callback);
        bool removePluginInstance(int32_t instanceId);

        // Direct plugin instance access
        AudioPluginHostPAL::AudioPluginNodePAL* getPluginInstance(int32_t instanceId);
        bool isPluginBypassed(int32_t instanceId);
        void setPluginBypassed(int32_t instanceId, bool bypassed);

        // Sequencer-level queries
        std::vector<ParameterUpdate> getParameterUpdates(int32_t instanceId);
        std::vector<int32_t> getInstanceIds();
        std::string getPluginName(int32_t instanceId);
        std::string getPluginFormat(int32_t instanceId);
        std::vector<TrackInfo> getTrackInfos();
        int32_t findTrackIndexForInstance(int32_t instanceId) const;

        // audio/MIDI player

        void sendNoteOn(int32_t targetId, int32_t note);
        void sendNoteOff(int32_t targetId, int32_t note);
        void setParameterValue(int32_t instanceId, int32_t index, double value);
        void enqueueUmp(int32_t targetId, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void enqueueUmpForInstance(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void setPluginOutputHandler(int32_t instanceId, PluginOutputHandler handler);
        std::optional<uint8_t> pluginGroup(int32_t instanceId) const;
        std::optional<int32_t> instanceForGroup(uint8_t group) const;

        // Audio controller (WIP, unused yet)

        uapmd_status_t startAudio();
        uapmd_status_t stopAudio();
        uapmd_status_t isAudioPlaying();

        // Transport control
        void startPlayback();
        void stopPlayback();
        void pausePlayback();
        void resumePlayback();
        int64_t playbackPositionSamples() const;

        int32_t sampleRate();
        bool sampleRate(int32_t newSampleRate);

        std::vector<uint8_t> saveState();
        void loadState(std::vector<uint8_t>& state);

        bool offlineRendering() const;
        void setOfflineRendering(bool enabled);
    };
}
