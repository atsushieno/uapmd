#pragma once

#include <vector>
#include "remidy/remidy.hpp"

namespace uapmd {
    class SequenceProcessContext {
        remidy::MasterContext master_context{};
    public:
        remidy::MasterContext& masterContext() { return master_context; }
        std::vector<remidy::AudioProcessContext*> tracks{};
    };
}