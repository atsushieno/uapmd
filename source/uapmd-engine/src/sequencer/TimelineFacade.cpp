#include "TimelineFacadeImpl.hpp"

namespace uapmd {
    std::unique_ptr<TimelineFacade> TimelineFacade::create(
        SequencerEngine& engine,
        ProjectHistoryFactory historyFactory) {
        return std::make_unique<TimelineFacadeImpl>(engine, std::move(historyFactory));
    }

} // namespace uapmd
