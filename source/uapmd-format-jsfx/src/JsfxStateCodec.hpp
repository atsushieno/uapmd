#pragma once

#include <cstdint>
#include <vector>

#include "ysfx.h"

namespace uapmd_jsfx {

    // The byte form a JSFX instance's state takes inside a project document.
    //
    // ysfx hands out state as a `ysfx_state_t`, which is a sparse list of slider values
    // plus whatever the script wrote in its `@serialize` section. It has no encoding of its
    // own, so this is ours, and because it ends up inside saved projects it is versioned
    // and byte order is fixed rather than native.
    //
    // Layout, all integers little-endian and all values IEEE-754 little-endian:
    //
    //     0   4   magic, "JSFX"
    //     4   4   uint32 version
    //     8   4   uint32 slider count
    //    12   n   slider count * { uint32 index, double value }
    //     .   8   uint64 serialized data size
    //     .   n   serialized data
    //
    // The slider list is sparse, as ysfx produces it. That matters when a script is edited
    // between sessions: values are matched by slider index rather than by position, so
    // adding a slider in the middle of a script does not shift everything after it.
    //
    // The serialized data is opaque. It is whatever the script's `@serialize` section
    // wrote, and only that script can interpret it.
    inline constexpr uint32_t kStateVersion = 1;

    struct DecodedState {
        std::vector<ysfx_state_slider_t> sliders{};
        std::vector<uint8_t> data{};
    };

    std::vector<uint8_t> encodeState(const ysfx_state_t& state);

    // Returns false when the bytes are not JSFX state, or are a version this build does not
    // understand. An empty input decodes to an empty state and succeeds, because a plugin
    // that was never edited legitimately has nothing to restore.
    bool decodeState(const std::vector<uint8_t>& bytes, DecodedState& out);

}
