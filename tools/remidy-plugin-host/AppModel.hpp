#pragma once

#include <uapmd/uapmd.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {
    class AppModel {
        const size_t buffer_size_in_frames;
        const size_t ump_buffer_size_in_bytes;
        int32_t sample_rate;
        DeviceIODispatcher dispatcher{};
        AudioPluginHostPAL* plugin_host_pal;
        SequenceProcessor sequencer;

        int32_t trackIndexForInstanceId(int32_t instance) {
            for (int32_t i = 0, n = sequencer.tracks().size(); i < n; i++) {
                auto track = sequencer.tracks()[i];
                for (auto plugin : track->graph().plugins())
                    if (plugin->instanceId() == instance)
                        return i;
            }
            return -1;
        }

    public:
        static AppModel& instance();
        AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, int32_t sampleRate);

        // Audio plugin support

        remidy::PluginCatalog& catalog() { return plugin_host_pal->catalog(); }

        void performPluginScanning(bool rescan) {
            plugin_host_pal->performPluginScanning(rescan);
        }

        std::vector<std::function<void(int32_t instancingId, int32_t instanceId, std::string)>> instancingCompleted{};

        void instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
            std::string formatString{format};
            std::string pluginIdString{pluginId};
            sequencer.addSimpleTrack(formatString, pluginIdString, [&,instancingId](AudioPluginTrack* track, std::string error) {
                // FIXME: error reporting instead of dumping out here
                if (!error.empty()) {
                    std::string msg = std::format("Instancing ID {}: {}", instancingId, error);
                    remidy::Logger::global()->logError(msg.c_str());
                }
                auto trackCtx = sequencer.data().tracks[sequencer.tracks().size() - 1];
                auto numChannels = dispatcher.audio()->channels();
                trackCtx->configureMainBus(numChannels, numChannels, buffer_size_in_frames);

                for (auto& f : instancingCompleted)
                    f(instancingId, track->graph().plugins()[0]->instanceId(), error);
            });
        }

        AudioPluginNode* getInstance(int32_t instanceId) {
            for (auto& track : sequencer.tracks())
                for (auto node : track->graph().plugins())
                    if (node->instanceId() == instanceId)
                        return node;
            return nullptr;
        }

        std::vector<ParameterMetadata> getParameterList(int32_t instanceId) {
            auto node = getInstance(instanceId);
            if (node)
                return node->pal()->parameterMetadataList();
            else
                return {};
        }

        // audio/MIDI player

        void sendNoteOn(int32_t instanceId, int32_t note);
        void sendNoteOff(int32_t instanceId, int32_t note);

        // Audio controller (WIP, unused yet)

        uapmd_status_t startAudio() {
            return dispatcher.start();
        }

        uapmd_status_t stopAudio() {
            return dispatcher.stop();
        }

        uapmd_status_t isAudioPlaying() {
            return dispatcher.isPlaying();
        }

        int32_t sampleRate() { return sample_rate; }
        bool sampleRate(int32_t newSampleRate) {
            if (dispatcher.isPlaying())
                return false;
            sample_rate = newSampleRate;
            return true;
        }
    };
}
