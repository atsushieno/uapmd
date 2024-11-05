#pragma once

#include <vector>
#include <remidy/remidy.hpp>

namespace uapmd {
    class SequenceData {
    public:
        std::vector<remidy::AudioProcessContext*> tracks{};
    };
}