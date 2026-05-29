#pragma once

#include <string_view>

namespace uapmd::builtin {

    inline constexpr std::string_view kGainNodeType = "webaudio:GainNode";
    inline constexpr std::string_view kChannelMergerNodeType = "webaudio:ChannelMergerNode";
    inline constexpr std::string_view kChannelSplitterNodeType = "webaudio:ChannelSplitterNode";

}
