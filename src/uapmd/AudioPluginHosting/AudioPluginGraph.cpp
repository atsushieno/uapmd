
#include "uapmd/uapmd.hpp"

#include <cassert>

namespace uapmd {

    class AudioPluginGraph::Impl {
        std::vector<AudioPluginNode*> nodes;

    public:
        int32_t processAudio(AudioProcessContext* process);
    };

    int32_t AudioPluginGraph::Impl::processAudio(AudioProcessContext* process) {
        // FIXME: implement
        assert(false);
    }

    AudioPluginGraph::AudioPluginGraph() {
        impl = new Impl();
    }

    AudioPluginGraph::~AudioPluginGraph() {
        delete impl;
    }

    int32_t AudioPluginGraph::processAudio(AudioProcessContext* process) {
        return impl->processAudio(process);
    }

}
