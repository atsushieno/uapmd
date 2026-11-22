#pragma once

#include <span>

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd_app_gui {

inline void setAllTracks(uapmd::ProjectCommands& commands, std::span<uapmd::SequencerTrack* const> tracks,
                         bool mute, bool enabled, const char* label) {
    uapmd::ScopedCommandStep step(commands.history(), label);
    if (!step.opened())
        return;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const auto* track = tracks[index];
        if (!track || (mute ? track->muted() : track->solo()) == enabled)
            continue;
        const auto trackIndex = static_cast<int32_t>(index);
        const bool applied = mute ? commands.setTrackMuted(trackIndex, enabled)
                                  : commands.setTrackSolo(trackIndex, enabled);
        if (!applied)
            return;
    }
    step.commit();
}

}
