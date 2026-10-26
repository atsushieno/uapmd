#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace uapmd {

    // Authored presentation metadata, owned and accessed by the model thread.
    // Paths are case-sensitive organizational strings; non-empty paths sort first.
    struct TrackProperties {
        std::optional<uint32_t> color; // 0xRRGGBB; absent uses the current theme.
        std::string path;
        double order{0.0};

        bool operator==(const TrackProperties&) const = default;

        bool valid() const {
            return std::isfinite(order) && (!color || *color <= 0xFFFFFF);
        }
    };

}
