#pragma once

#include <uapmd/uapmd.hpp>
#include <remidy-tooling/PluginScanning.hpp>
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {
    class AppModel {
        const int32_t ump_buffer_size;
        int32_t sample_rate;
        DeviceIODispatcher dispatcher;
        AudioPluginHostPAL* plugin_host_pal;
        AudioPluginSequencer sequencer;
        remidy::MasterContext masterContext{};
        std::vector<remidy::TrackContext*> trackContexts{};
        std::vector<remidy::AudioProcessContext*> trackBuffers{};

    public:
        static AppModel& instance();

        AppModel(size_t umpBufferSize, int32_t sampleRate) :
            ump_buffer_size(umpBufferSize), sample_rate(sampleRate),
            plugin_host_pal(AudioPluginHostPAL::instance()),
            sequencer(sampleRate, this->plugin_host_pal),
            dispatcher(umpBufferSize) {

            dispatcher.addCallback([&](uapmd::AudioProcessContext& process) {
                uapmd::SequenceData data{ .tracks = trackBuffers };
                for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
                    auto track = sequencer.tracks()[t];
                    auto ctx = data.tracks[t];
                    for (int32_t i = 0, n = process.audioInBusCount(); i < n; ++i)
                        ctx->addAudioIn(dispatcher.audioDriver()->channels(), 1024);
                    for (int32_t i = 0, n = process.audioOutBusCount(); i < n; ++i)
                        ctx->addAudioOut(dispatcher.audioDriver()->channels(), 1024);
                    ctx->frameCount(512);
                    for (uint32_t i = 0; i < process.audioInBusCount(); i++) {
                        auto srcInBus = process.audioIn(i);
                        auto dstInBus = ctx->audioIn(i);
                        for (uint32_t ch = 0, nCh = srcInBus->channelCount(); ch < nCh; ch++)
                            memcpy(dstInBus->getFloatBufferForChannel(ch), (void*) srcInBus->getFloatBufferForChannel(ch), process.frameCount());
                    }
                    for (uint32_t i = 0; i < process.audioOutBusCount(); i++) {
                        auto dstOutBus = process.audioOut(i);
                        auto srcOutBus = ctx->audioOut(i);
                        for (uint32_t ch = 0, nCh = srcOutBus->channelCount(); ch < nCh; ch++)
                            memcpy(dstOutBus->getFloatBufferForChannel(ch), (void*) srcOutBus->getFloatBufferForChannel(ch), process.frameCount());
                    }
                }
                return sequencer.processAudio(data);
            });
        }

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
                for (auto& f : instancingCompleted)
                    f(instancingId, track->graph().plugins()[0]->instanceId(), error);
                auto trackCtx = new remidy::TrackContext(masterContext);
                trackContexts.emplace_back(trackCtx);
                trackBuffers.emplace_back(new remidy::AudioProcessContext(4096, trackCtx));
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

        void sendNoteOn(int32_t instanceId, int32_t note);
        void sendNoteOff(int32_t instanceId, int32_t note);
    };
}
