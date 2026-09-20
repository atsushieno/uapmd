#pragma once

namespace uapmd { class TimelineFacade; }
namespace uapmd_addin { class PanelRegistry; }

namespace uapmd_augene2 {

// Project persistence is available even when the addin's UI/watch service is
// disabled. The application releases it before destroying the timeline.
void registerProjectService(uapmd::TimelineFacade& timeline, uapmd_addin::PanelRegistry& panels);

}
