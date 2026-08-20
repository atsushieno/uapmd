#include "TimelineFacadeImpl.hpp"

// Track mixer state, device input routing and plug-in graph wiring.

namespace uapmd {
    std::optional<TrackGraphSnapshot> TimelineFacadeImpl::captureTrackGraphSnapshot(
            int32_t trackIndex) {
                auto* track = resolveSequencerTrack(trackIndex);
        if (!track)
            return std::nullopt;
        auto* provider = audio_graph_provider_registry_.get(track->graph());
        if (!provider)
            return std::nullopt;
        auto graphData = createSerializedProjectGraph(
            *provider,
            track->orderedInstanceIds(),
            track->graph(),
            [this](int32_t instanceId) {
                return engine_.getPluginInstance(instanceId);
            },
            nullptr);
        if (!graphData)
            return std::nullopt;
        TrackGraphSnapshot snapshot;
        snapshot.graphType = provider->id();
        if (!provider->saveProjectGraph(
                graphData.get(), snapshot.graphBytes))
            return std::nullopt;
        return snapshot;
    }

    bool TimelineFacadeImpl::applyTrackGraphSnapshot(
            std::string_view trackReferenceId,
            const TrackGraphSnapshot& snapshot,
            size_t eventBufferSizeInBytes) {
                auto* provider =
            audio_graph_provider_registry_.get(snapshot.graphType);
        const auto trackIndex =
            trackIndexForPersistentId(trackReferenceId);
        if (!provider
            || (trackIndex < 0 && trackIndex != kMasterTrackIndex))
            return false;
        auto newGraph = provider->createGraph(eventBufferSizeInBytes);
        if (!newGraph)
            return false;
        ++suppress_plugin_graph_notifications_;
        const auto replaced = engine_.replaceTrackGraph(
            trackIndex,
            std::move(newGraph));
        --suppress_plugin_graph_notifications_;
        if (!replaced)
            return false;
        if (snapshot.graphBytes.empty()) {
            onTrackGraphChanged(trackIndex);
            return true;
        }

        auto* track = resolveSequencerTrack(trackIndex);
        auto metadata = UapmdProjectPluginGraphData::create();
        if (!track || !metadata)
            return false;
        metadata->graphType(snapshot.graphType);
        auto graphData = loadSerializedProjectGraph(
            *provider,
            *metadata,
            snapshot.graphBytes);
        if (!graphData
            || !provider->deserializeRuntimeGraph(
                graphData.get(),
                track->graph(),
                track->orderedInstanceIds()))
            return false;
        onTrackGraphChanged(trackIndex);
        return true;
    }

    bool TimelineFacadeImpl::applyDeviceInputState(
            std::string_view trackReferenceId,
            int32_t sourceNodeId,
            const std::optional<std::vector<uint32_t>>& channels) {
                auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
        if (!targetTrack)
            return false;
        auto source = targetTrack->getSourceNode(sourceNodeId);
        auto deviceInput =
            std::dynamic_pointer_cast<DeviceInputSourceNode>(source);
        if (!channels) {
            if (!deviceInput || !targetTrack->removeSource(sourceNodeId))
                return false;
        } else if (deviceInput)
            deviceInput->setInputChannels(*channels);
        else {
            if (source)
                return false;
            const auto channelCount = static_cast<uint32_t>(channels->size());
            auto newSource = std::make_unique<DeviceInputSourceNode>(
                sourceNodeId,
                channelCount,
                *channels);
            if (!targetTrack->addDeviceInputSource(std::move(newSource)))
                return false;
        }
        emitTrackChanged(trackReferenceId, "track-device-input-changed");
        notifyTimelineChanged();
        return true;
    }

    std::optional<std::vector<uint32_t>> TimelineFacadeImpl::deviceInputChannels(
        std::string_view trackReferenceId,
        int32_t sourceNodeId) {
        auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
        if (!targetTrack)
            return std::nullopt;
        auto deviceInput = std::dynamic_pointer_cast<DeviceInputSourceNode>(
            targetTrack->getSourceNode(sourceNodeId));
        if (!deviceInput)
            return std::nullopt;
        return deviceInput->getInputChannels();
    }

    bool TimelineFacadeImpl::applyGraphConnectionState(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& desired,
            bool present,
            std::string& error) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        auto* track = resolveSequencerTrack(trackIndex);
        auto* graph = track
            ? track->graph().getExtension<uapmd_graph::GraphConnectionExtension>()
            : nullptr;
        if (!graph) {
            error = "Track graph does not support connection editing";
            return false;
        }

        const auto connections = graph->connections();
        auto existing = std::find_if(
            connections.begin(),
            connections.end(),
            [&desired](const auto& connection) {
                return graphConnectionEquivalent(connection, desired);
            });
        if (!present) {
            if (existing == connections.end()) {
                error = "Graph connection not found";
                return false;
            }
            if (!graph->disconnect(existing->id)) {
                error = "Failed to disconnect graph endpoints";
                return false;
            }
        } else {if (existing != connections.end())
                return true;
            auto connection = desired;
            connection.id = 0;
            auto resolveEndpoint = [this, trackReferenceId](
                                       auto& endpoint) {
                if (endpoint.type
                    != uapmd_graph::AudioPluginGraphEndpointType::Plugin)
                    return true;
                endpoint.instance_id = resolvePluginInstanceId(
                    trackReferenceId,
                    endpoint.node_id);
                return endpoint.instance_id >= 0;
            };
            if (!resolveEndpoint(connection.source)
                || !resolveEndpoint(connection.target)) {
                error = "A plug-in graph endpoint no longer exists";
                return false;
            }
            const auto result = graph->connect(connection);
            if (result != 0) {
                if (result == -1)
                    error = "Invalid graph endpoint direction";
                else if (result == -2)
                    error = "Graph endpoint does not exist";
                else if (result == -3)
                    error = "Graph connection would create a cycle";
                else
                    error = "Failed to connect graph endpoints";
                return false;
            }
        }
        onTrackGraphChanged(trackIndex);
        notifyTimelineChanged();
        return true;
    }

    bool TimelineFacadeImpl::graphConnectionPresent(
        std::string_view trackReferenceId,
        const uapmd_graph::AudioPluginGraphConnection& connection) {
        const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        auto* track = resolveSequencerTrack(trackIndex);
        auto* graph = track
            ? track->graph().getExtension<uapmd_graph::GraphConnectionExtension>()
            : nullptr;
        if (!graph)
            return false;
        const auto connections = graph->connections();
        return std::any_of(
            connections.begin(),
            connections.end(),
            [&connection](const auto& candidate) {
                return graphConnectionEquivalent(candidate, connection);
            });
    }

    std::optional<uapmd_graph::AudioPluginGraphConnection>
    TimelineFacadeImpl::graphConnectionById(int32_t trackIndex, int64_t connectionId) {
        auto* track = resolveSequencerTrack(trackIndex);
        auto* graph = track
            ? track->graph().getExtension<uapmd_graph::GraphConnectionExtension>()
            : nullptr;
        if (!graph)
            return std::nullopt;
        const auto connections = graph->connections();
        auto found = std::find_if(
            connections.begin(),
            connections.end(),
            [connectionId](const auto& candidate) {
                return candidate.id == connectionId;
            });
        if (found == connections.end())
            return std::nullopt;
        return *found;
    }

    bool TimelineFacadeImpl::setTrackFreezePolicy(int32_t index, bool enabled) {
        return engine_.frozenTrackManager().setFreezePolicyForTrack(
            index,
            enabled
                ? FrozenTrackManager::FreezePolicy::On
                : FrozenTrackManager::FreezePolicy::Off);
    }

    bool TimelineFacadeImpl::trackFreezePolicyEnabled(int32_t index) const {
        return engine_.frozenTrackManager().freezePolicyForTrack(index)
            == FrozenTrackManager::FreezePolicy::On;
    }

    void TimelineFacadeImpl::onTrackChanged(
            std::string_view trackReference,
            std::string_view changeType) {
                emitTrackChanged(trackReference, std::string(changeType));
    }

    void TimelineFacadeImpl::onTrackMutated(
            std::string_view trackReference,
            std::string_view changeType) {
                emitTrackChanged(trackReference, std::string(changeType));
        notifyTimelineChanged();
    }

    void TimelineFacadeImpl::onClipMutated(
            TimelineTrack& track,
            int32_t clipIdentifier,
            std::string_view changeType) {
                auto* clip = track.clipManager().getClip(clipIdentifier);
        if (!clip)
            return;
        emitClipChanged(track, *clip, std::string(changeType));
        if (clip->clipType == ClipType::Midi)
            emitMasterTrackChanged("master-track-content-changed");
        notifyTimelineChanged();
    }

    void TimelineFacadeImpl::resolveClipAnchors() {
        resolveAllClipAnchors();
    }

} // namespace uapmd
