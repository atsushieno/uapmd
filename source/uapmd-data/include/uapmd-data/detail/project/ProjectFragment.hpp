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

    // One plugin on a captured track. Instances cannot be copied, so a fragment
    // records what is needed to instantiate an equivalent one and restore its
    // state into it.
    struct ProjectTrackPluginFragment {
        // Persistent graph node identity. Reused under
        // ProjectObjectIdPolicy::Restore so that anything keyed by it, notably
        // an ARA archive, reconnects to this plugin.
        std::string nodeId{};
        std::string pluginId{};
        std::string format{};
        std::string displayName{};
        int32_t groupIndex{-1};
        // Opaque plugin state. Reading it is asynchronous, which is why
        // capturing a track fragment cannot be a plain return value.
        std::vector<uint8_t> state{};
    };

    // Which parts of a captured track are applied when it is attached.
    //
    // Track duplication is not all-or-nothing in any DAW that offers it: the
    // common choices are a full clone, and a track set up the same way but
    // empty. Capture always takes everything, so one fragment serves a clone, a
    // partial duplicate and an undo restore without being recaptured.
    struct ProjectTrackAttachOptions {
        ProjectObjectIdPolicy idPolicy{ProjectObjectIdPolicy::Mint};
        // Negative appends. Undo restoration supplies the removed track's
        // former index so project ordering is restored as well as identity.
        int32_t insertionIndex{-1};
        bool includePlugins{true};
        // Instantiate the plugins but leave them at their defaults. Skipping
        // state also skips the slowest part of attaching.
        bool includePluginState{true};
        bool includeClips{true};
    };

    // A track detached from the document.
    //
    // Unlike a clip, a track owns live plugin instances rather than plain data,
    // so both capturing and attaching one are asynchronous.
    struct ProjectTrackFragment {
        std::string referenceId{};
        double volume{1.0};
        bool muted{false};
        bool solo{false};

        // Graph topology as the track's provider serializes it, plus the
        // provider identifier needed to deserialize it again. Held by value:
        // the .graph.json file written during a project save is an artifact of
        // saving, not of the serialization.
        std::string graphType{};
        std::vector<uint8_t> graphBytes{};

        std::vector<ProjectTrackPluginFragment> plugins{};
        std::vector<ProjectClipFragment> clips{};

        // Opaque per-feature state covering the track itself, keyed by
        // extension identifier, as with a clip fragment.
        std::map<std::string, std::vector<uint8_t>> extensionState{};
    };

} // namespace uapmd
