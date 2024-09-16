
#include "AudioPluginNode.hpp"

#include <string>

namespace uapmd {

    class AudioPluginNode::Impl {
    public:
        std::string pluginId;
        bool bypassed;
    };

    bool AudioPluginNode::isBypassed() { return impl->bypassed; }

    void AudioPluginNode::setBypassed(bool value) { impl->bypassed = value;; }

    AudioPluginNode::AudioPluginNode(const char *pluginId) {
        impl = new Impl();
        impl->pluginId = pluginId;
    }

    AudioPluginNode::~AudioPluginNode() {
        delete impl;
    }

    const char * AudioPluginNode::getPluginId() const {
        return impl->pluginId.c_str();
    }

}
