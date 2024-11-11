#pragma once

namespace uapmd {
    class AudioPluginNode;

    // This PAL is more like a Plugin(-Format) Abstraction Layer rather than Platform Abstraction Layer.
    class AudioPluginHostPAL {
    public:
        class AudioPluginNodePAL {
        public:
            virtual ~AudioPluginNodePAL() = default;
            virtual std::string& formatName() const = 0;
            virtual std::string& pluginId() const = 0;
            virtual uapmd_status_t processAudio(AudioProcessContext &process) = 0;
        };

        static AudioPluginHostPAL* instance();
        virtual ~AudioPluginHostPAL() = default;
        virtual void createPluginInstance(uint32_t sampleRate, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) = 0;
    };

}