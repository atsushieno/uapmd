#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../midi/MidiTimelineEvents.hpp"
#include "../timeline/TimelineTypes.hpp"

namespace uapmd {

    // Whether an object being attached to the document keeps the identifiers it
    // already carries, or is given fresh ones.
    enum class ProjectObjectIdPolicy {
        // Reuse the fragment's identifiers. Restoring something that was
        // removed has to bring it back as the same object, or anything keyed by
        // its identity -- ARA persistent IDs, frozen renders -- will not
        // reconnect to it.
        Restore,
        // Allocate fresh identifiers. Used when the fragment becomes an
        // additional object rather than a restored one.
        Mint
    };

    // A clip detached from the document: everything needed to recreate it,
    // independent of the track it came from.
    //
    // This is the payload for both an undo entry and a clipboard entry. The two
    // differ only in the ProjectObjectIdPolicy used to attach the fragment back.
    struct ProjectClipFragment {
        // Clip metadata, exactly as the document holds it. This carries the
        // identifiers that ProjectObjectIdPolicy::Restore reuses, along with
        // name, gain, mute, enablement, markers, warps and anchoring.
        ClipData clip{};

        // Authored MIDI content. Empty for audio clips, which are rebuilt from
        // `clip.filepath`. Tick resolution and clip tempo are not repeated here
        // because `clip` already carries them.
        std::vector<uapmd_ump_t> umpEvents{};
        std::vector<uint64_t> umpTickTimestamps{};
        std::vector<MidiTempoChange> tempoChanges{};
        std::vector<MidiTimeSignatureChange> timeSignatureChanges{};

        // Opaque per-feature state covering this clip, keyed by the same
        // identifier a ProjectSerializationExtension uses. An ARA partial
        // archive belongs here: the host can neither read nor diff that state,
        // so a fragment has to carry it verbatim to restore it faithfully.
        std::map<std::string, std::vector<uint8_t>> extensionState{};

        bool isMidi() const {
            return clip.clipType == ClipType::Midi;
        }
    };

} // namespace uapmd
