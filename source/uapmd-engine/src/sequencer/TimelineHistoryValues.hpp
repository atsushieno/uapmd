#pragma once

// Retained-size accounting and value comparison for objects held in undo history.
//
// Private to the timeline facade implementation; not part of the module's
// public surface. Included only by TimelineFacade.cpp.

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd::timeline_detail {

    inline size_t retainedValueSize(const std::string& value) {
        return sizeof(value) + value.capacity();
    }

    inline size_t retainedValueSize(const ClipMarker& value) {
        return sizeof(value)
            + value.markerId.capacity()
            + value.referenceClipId.capacity()
            + value.referenceMarkerId.capacity()
            + value.name.capacity();
    }

    inline size_t retainedValueSize(const AudioWarpPoint& value) {
        return sizeof(value)
            + value.referenceClipId.capacity()
            + value.referenceMarkerId.capacity();
    }

    inline size_t retainedValueSize(const TimeReference& value) {
        return sizeof(value) + value.referenceId.capacity();
    }

    inline size_t retainedValueSize(const ClipAddress& value) {
        return sizeof(value)
            + value.trackReferenceId.capacity()
            + value.clipReferenceId.capacity();
    }

    inline size_t retainedValueSize(const PluginAddress& value) {
        return sizeof(value)
            + value.trackReferenceId.capacity()
            + value.nodeId.capacity();
    }

    inline size_t retainedValueSize(const DeviceInputAddress& value) {
        return sizeof(value) + value.trackReferenceId.capacity();
    }

    // Graph connections are compared by what they connect, never by id: the
    // id is minted by the graph on connect, so a reconnected edge is a
    // different id for the same document-level connection.
    inline bool graphEndpointEquivalent(
        const uapmd_graph::AudioPluginGraphEndpoint& lhs,
        const uapmd_graph::AudioPluginGraphEndpoint& rhs) {
        return lhs.type == rhs.type
            && lhs.node_id == rhs.node_id
            && lhs.bus_index == rhs.bus_index;
    }

    inline bool graphConnectionEquivalent(
        const uapmd_graph::AudioPluginGraphConnection& lhs,
        const uapmd_graph::AudioPluginGraphConnection& rhs) {
        return lhs.bus_type == rhs.bus_type
            && graphEndpointEquivalent(lhs.source, rhs.source)
            && graphEndpointEquivalent(lhs.target, rhs.target);
    }

    // Which track a connection belongs to, plus the connection itself: the
    // pair is the address, and whether it is present is the value.
    struct GraphConnectionAddress {
        ProjectObjectId trackReferenceId;
        uapmd_graph::AudioPluginGraphConnection connection;

        bool operator==(const GraphConnectionAddress& other) const {
            return trackReferenceId == other.trackReferenceId
                && graphConnectionEquivalent(connection, other.connection);
        }
    };

    inline size_t retainedValueSize(const GraphConnectionAddress& value) {
        return sizeof(value)
            + value.trackReferenceId.capacity()
            + value.connection.source.node_id.capacity()
            + value.connection.target.node_id.capacity();
    }

    template<typename Value>
    size_t retainedValueSize(const Value&) {
        return sizeof(Value);
    }

    template<typename Value>
    size_t retainedValueSize(const std::vector<Value>& values) {
        size_t result = sizeof(values);
        for (const auto& value : values)
            result += retainedValueSize(value);
        return result;
    }

    inline size_t retainedValueSize(const ProjectClipFragment& fragment) {
        size_t result = sizeof(fragment)
            + fragment.clip.referenceId.capacity()
            + fragment.clip.name.capacity()
            + fragment.clip.filepath.capacity()
            + fragment.clip.anchorReferenceId.capacity()
            + retainedValueSize(fragment.clip.markers)
            + retainedValueSize(fragment.clip.audioWarps)
            + fragment.umpEvents.capacity() * sizeof(uapmd_ump_t)
            + fragment.umpTickTimestamps.capacity() * sizeof(uint64_t)
            + fragment.tempoChanges.capacity() * sizeof(MidiTempoChange)
            + fragment.timeSignatureChanges.capacity() * sizeof(MidiTimeSignatureChange);
        for (const auto& [extensionId, state] : fragment.extensionState)
            result += sizeof(extensionId) + extensionId.capacity()
                + sizeof(state) + state.capacity();
        return result;
    }

    // Everything needed to put one plug-in back exactly as it was: which
    // plug-in, how it was configured, its opaque state, and the graph edges
    // that touched it.
    struct PluginInstanceSnapshot {
        std::string format;
        std::string pluginId;
        bool bypassed{false};
        uint8_t group{0};
        std::vector<uint8_t> state;
        std::vector<uapmd_graph::AudioPluginGraphConnection> connections;
    };

    inline size_t retainedValueSize(const PluginInstanceSnapshot& snapshot) {
        size_t result = sizeof(snapshot)
            + snapshot.format.capacity()
            + snapshot.pluginId.capacity()
            + snapshot.state.capacity()
            + snapshot.connections.capacity()
                * sizeof(uapmd_graph::AudioPluginGraphConnection);
        for (const auto& connection : snapshot.connections)
            result += connection.source.node_id.capacity()
                + connection.target.node_id.capacity();
        return result;
    }

    inline size_t retainedValueSize(const ProjectTrackFragment& fragment) {
        size_t result = sizeof(fragment)
            + fragment.referenceId.capacity()
            + fragment.graphType.capacity()
            + fragment.graphBytes.capacity();
        for (const auto& plugin : fragment.plugins)
            result += sizeof(plugin)
                + plugin.nodeId.capacity()
                + plugin.pluginId.capacity()
                + plugin.format.capacity()
                + plugin.displayName.capacity()
                + plugin.state.capacity();
        for (const auto& clip : fragment.clips)
            result += retainedValueSize(clip);
        for (const auto& [extensionId, state] : fragment.extensionState)
            result += sizeof(extensionId) + extensionId.capacity()
                + sizeof(state) + state.capacity();
        return result;
    }

    inline size_t retainedValueSize(
        const LatencyCompensationProjectSettings& settings) {
        size_t result = sizeof(settings)
            + settings.implementation_id.capacity()
            + settings.monitored_track_indexes.capacity() * sizeof(int32_t)
            + settings.record_armed_track_indexes.capacity() * sizeof(int32_t);
        for (const auto& [key, value] : settings.implementation_properties)
            result += sizeof(key) + key.capacity()
                + sizeof(value) + value.capacity();
        return result;
    }

    inline bool latencyCompensationSettingsEqual(
        const LatencyCompensationProjectSettings& lhs,
        const LatencyCompensationProjectSettings& rhs) {
        return lhs.implementation_id == rhs.implementation_id
            && lhs.playback_compensation_mode == rhs.playback_compensation_mode
            && lhs.input_monitoring_policy == rhs.input_monitoring_policy
            && lhs.monitored_track_indexes == rhs.monitored_track_indexes
            && lhs.record_armed_track_indexes == rhs.record_armed_track_indexes
            && lhs.implementation_properties == rhs.implementation_properties;
    }

    struct TrackGraphSnapshot {
        std::string graphType;
        std::vector<uint8_t> graphBytes;
    };

    inline size_t retainedValueSize(const TrackGraphSnapshot& snapshot) {
        return sizeof(snapshot)
            + snapshot.graphType.capacity()
            + snapshot.graphBytes.capacity();
    }

    inline bool clipMarkerEqual(const ClipMarker& lhs, const ClipMarker& rhs) {
        return lhs.markerId == rhs.markerId
            && lhs.clipPositionOffset == rhs.clipPositionOffset
            && lhs.referenceType == rhs.referenceType
            && lhs.referenceClipId == rhs.referenceClipId
            && lhs.referenceMarkerId == rhs.referenceMarkerId
            && lhs.name == rhs.name;
    }

    inline bool audioWarpPointEqual(const AudioWarpPoint& lhs, const AudioWarpPoint& rhs) {
        return lhs.clipPositionOffset == rhs.clipPositionOffset
            && lhs.speedRatio == rhs.speedRatio
            && lhs.referenceType == rhs.referenceType
            && lhs.referenceClipId == rhs.referenceClipId
            && lhs.referenceMarkerId == rhs.referenceMarkerId;
    }

    inline bool clipMarkersEqual(const std::vector<ClipMarker>& lhs, const std::vector<ClipMarker>& rhs) {
        return lhs.size() == rhs.size()
            && std::equal(lhs.begin(), lhs.end(), rhs.begin(), clipMarkerEqual);
    }

    inline bool audioWarpPointsEqual(
        const std::vector<AudioWarpPoint>& lhs,
        const std::vector<AudioWarpPoint>& rhs) {
        return lhs.size() == rhs.size()
            && std::equal(lhs.begin(), lhs.end(), rhs.begin(), audioWarpPointEqual);
    }

} // namespace uapmd::timeline_detail
