#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace uapmd {

    inline constexpr std::string_view kStereoBalancePannerType = "uapmd:StereoBalance";
    inline constexpr std::string_view kChannelMatrixPannerType = "uapmd:ChannelMatrixPanner";

    struct TrackOutputPanner {
        std::string node_type{};
        // A JSON object, including structured parameters such as channel matrices.
        // Keep parameters owned by the node type so unknown types can round-trip.
        std::string parameters_json{"{}"};

        bool operator==(const TrackOutputPanner&) const = default;
    };

    // One panner node per zero-based track output bus.
    using TrackOutputPanners = std::map<uint32_t, TrackOutputPanner>;

}
