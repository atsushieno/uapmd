#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"
#include "../node-graph/AudioPluginGraph.hpp"

namespace uapmd {

    class SequencerTrack {
    protected:
        SequencerTrack() = default;

    public:
        virtual ~SequencerTrack() = default;
        static std::unique_ptr<SequencerTrack> create(size_t eventBufferSizeInBytes);

        virtual AudioPluginGraph& graph() = 0;

        virtual bool bypassed() = 0;
        virtual bool frozen() = 0;
        virtual void bypassed(bool value) = 0;
        virtual void frozen(bool value) = 0;
    };

}
