
#include "uapmd/uapmd.hpp"


#include <string>

namespace uapmd {

    class AudioPluginNode::Impl {
    public:
        Impl(std::string& formatName, std::string& pluginId) :
            formatName(formatName), pluginId(pluginId) {
        }
        std::string formatName;
        std::string pluginId;
        bool bypassed;

        uapmd_status_t processAudio(AudioProcessContext &process);
    };

    bool AudioPluginNode::bypassed() { return impl->bypassed; }

    void AudioPluginNode::bypassed(bool value) { impl->bypassed = value;; }

    AudioPluginNode::AudioPluginNode(std::string& formatName, std::string& pluginId) {
        impl = new Impl(formatName, pluginId);
    }

    AudioPluginNode::~AudioPluginNode() {
        delete impl;
    }

    std::string& AudioPluginNode::formatName() const {
        return impl->formatName;
    }

    std::string& AudioPluginNode::pluginId() const {
        return impl->pluginId;
    }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        return impl->processAudio(process);
    }

    uapmd_status_t AudioPluginNode::Impl::processAudio(AudioProcessContext &process) {
        throw std::runtime_error("Not implemented");
    }
}
