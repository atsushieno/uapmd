#pragma once

#include <memory>
#include <string_view>

#include "AudioGraphDescriptor.hpp"
#include "AudioGraphNode.hpp"

namespace uapmd_graph {

    class AudioGraphBuiltInNodeFactory {
    public:
        virtual ~AudioGraphBuiltInNodeFactory() = default;

        virtual std::string_view nodeType() const = 0;
        virtual std::unique_ptr<AudioGraphNode> create(const AudioGraphNodeDescriptor& descriptor) const = 0;
    };

}
