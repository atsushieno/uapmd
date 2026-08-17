#include "TimelineFacadeImpl.hpp"

namespace uapmd {
    std::unique_ptr<TimelineFacade> TimelineFacade::create(SequencerEngine& engine) {
        return std::make_unique<TimelineFacadeImpl>(engine);
    }

} // namespace uapmd
