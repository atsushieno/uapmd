#pragma once

namespace uapmd_app_gui {

// The snap divisions the piano roll and the timeline share. Each label denotes a fraction of a
// whole note, so (for example) "1/16" is a sixteenth-note grid.
constexpr const char* kSnapLabels[] = {
    "Free", "1/8", "1/16", "1/24", "1/32", "1/48", "1/64",
};
// Durations are expressed in quarter-note beats. Each label denotes a fraction
// of a whole note, so (for example) a 1/16 note spans 4/16 of a quarter-note beat.
constexpr float kSnapValues[] = {
    0.0f, 4.0f / 8.0f, 4.0f / 16.0f, 4.0f / 24.0f,
    4.0f / 32.0f, 4.0f / 48.0f, 4.0f / 64.0f,
};
constexpr int kSnapOptionCount = static_cast<int>(sizeof(kSnapLabels) / sizeof(kSnapLabels[0]));
static_assert(kSnapOptionCount == static_cast<int>(sizeof(kSnapValues) / sizeof(kSnapValues[0])));
// 1/16, in both editors.
constexpr int kDefaultSnapIndex = 2;

} // namespace uapmd_app_gui
