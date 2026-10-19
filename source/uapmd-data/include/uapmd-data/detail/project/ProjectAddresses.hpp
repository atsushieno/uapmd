#pragma once

#include <cstdint>
#include <string>
#include "ProjectDocumentEvents.hpp"

namespace uapmd {
    // Stable document identity of one clip.
    struct ClipAddress {
        ProjectObjectId trackReferenceId;
        ProjectObjectId clipReferenceId;

        bool operator==(const ClipAddress&) const = default;
    };

    // Stable document identity of one plug-in node. A runtime instance id is
    // not usable in history: removing and restoring a plug-in produces a new
    // instance id for the same document object.
    struct PluginAddress {
        ProjectObjectId trackReferenceId;
        std::string nodeId;

        bool operator==(const PluginAddress&) const = default;
    };

    // Stable document identity of one device input on a track. The source
    // node id is minted per track and persisted with it, so it survives the
    // input being removed and restored.
    struct DeviceInputAddress {
        ProjectObjectId trackReferenceId;
        int32_t sourceNodeId{-1};

        bool operator==(const DeviceInputAddress&) const = default;
    };

}
