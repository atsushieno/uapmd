#pragma once

#include <vector>
#include "uapmd-midi-service/uapmd-midi-service.hpp"

namespace uapmd {
    class SequenceProcessContext {
        MasterContext master_context{};
    public:
        MasterContext& masterContext() { return master_context; }
        std::vector<AudioProcessContext*> tracks{};
    };
}