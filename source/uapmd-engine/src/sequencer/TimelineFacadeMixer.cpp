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

    bool TimelineFacadeImpl::replaceTrackGraphType(
            int32_t trackIndex,
            const std::string& graphTypeId,
            size_t eventBufferSizeInBytes,
            ProjectMutationOrigin origin) {
                auto* provider = audio_graph_provider_registry_.get(graphTypeId);
        if (!provider)
            return false;

        SequencerTrack* track = trackIndex == kMasterTrackIndex
            ? engine_.masterTrack()
            : (trackIndex >= 0 && trackIndex < static_cast<int32_t>(engine_.tracks().size())
                ? engine_.tracks()[static_cast<size_t>(trackIndex)]
                : nullptr);
        if (!track)
            return false;

        if (track->graph().providerId() == provider->id())
            return true;
        auto* timelineTrack = resolveTrack(trackIndex);
        if (!timelineTrack)
            return false;
        auto before = captureTrackGraphSnapshot(trackIndex);
        if (!before)
            return false;
        auto trackReferenceId = timelineTrack->referenceId();
        TrackGraphSnapshot requested;
        requested.graphType = graphTypeId;
        if (!applyTrackGraphSnapshot(
                trackReferenceId,
                requested,
                eventBufferSizeInBytes))
            return false;
        if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote)
            return true;

        auto after = captureTrackGraphSnapshot(trackIndex);
        if (!after) {
            applyTrackGraphSnapshot(
                trackReferenceId, *before, eventBufferSizeInBytes);
            return false;
        }
        auto apply = [this, eventBufferSizeInBytes](
                         std::string_view persistentTrackId,
                         const TrackGraphSnapshot& desired,
                         const TrackGraphSnapshot& compensation) {
            if (applyTrackGraphSnapshot(
                    persistentTrackId,
                    desired,
                    eventBufferSizeInBytes))
                return true;
            applyTrackGraphSnapshot(
                persistentTrackId,
                compensation,
                eventBufferSizeInBytes);
            return false;
        };
        auto operation = std::make_shared<TrackGraphUndoOperation>(
            trackReferenceId,
            *before,
            *after,
            std::move(apply));
        auto result = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.recordPerformed(
            std::move(operation),
            origin,
            [result](ProjectUndoResult completed) {
                *result = std::move(completed);
            });
        if (result->has_value() && result->value().succeeded())
            return true;
        applyTrackGraphSnapshot(
            trackReferenceId, *before, eventBufferSizeInBytes);
        return false;
    }

    bool TimelineFacadeImpl::setLatencyCompensationSettings(
            const LatencyCompensationProjectSettings& settings,
            ProjectMutationOrigin origin) {
                auto* manager = engine_.latencyCompensationManager();
        if (!manager)
            return false;
        auto before = manager->projectSettings();
        if (latencyCompensationSettingsEqual(before, settings))
            return true;

        auto apply = [this](
                         const LatencyCompensationProjectSettings& value,
                         std::string& error) {
            auto* currentManager = engine_.latencyCompensationManager();
            if (!currentManager
                || !currentManager->applyProjectSettings(value, error))
                return false;
            notifyTimelineChanged();
            return true;
        };
        if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote) {
            std::string error;
            return apply(settings, error);
        }

        auto operation = std::make_shared<LatencySettingsUndoOperation>(
            std::move(before),
            settings,
            std::move(apply));
        auto result = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.perform(
            std::move(operation),
            origin,
            [result](ProjectUndoResult completed) {
                *result = std::move(completed);
            });
        return result->has_value() && result->value().succeeded();
    }

    bool TimelineFacadeImpl::applyDeviceInputState(
            std::string_view trackReferenceId,
            int32_t sourceNodeId,
            const DeviceInputUndoOperation::Channels& channels) {
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

    bool TimelineFacadeImpl::performDeviceInputMutation(
            int32_t trackIndex,
            int32_t sourceNodeId,
            DeviceInputUndoOperation::Channels before,
            DeviceInputUndoOperation::Channels after,
            ProjectMutationOrigin origin,
            std::string description) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        auto trackReferenceId = targetTrack->referenceId();
        auto apply = [this](
                         std::string_view persistentTrackId,
                         int32_t persistentSourceNodeId,
                         const DeviceInputUndoOperation::Channels& value) {
            return applyDeviceInputState(
                persistentTrackId,
                persistentSourceNodeId,
                value);
        };
        if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote)
            return apply(trackReferenceId, sourceNodeId, after);

        auto operation = std::make_shared<DeviceInputUndoOperation>(
            std::move(description),
            std::move(trackReferenceId),
            sourceNodeId,
            std::move(before),
            std::move(after),
            std::move(apply));
        auto result = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.perform(
            std::move(operation),
            origin,
            [result](ProjectUndoResult completed) {
                *result = std::move(completed);
            });
        return result->has_value() && result->value().succeeded();
    }

    bool TimelineFacadeImpl::addDeviceInputToTrack(
            int32_t trackIndex,
            int32_t sourceNodeId,
            const std::vector<uint32_t>& channelIndices,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack || targetTrack->getSourceNode(sourceNodeId))
            return false;
        auto normalizedChannels = channelIndices;
        if (normalizedChannels.empty())
            normalizedChannels = {0, 1};
        return performDeviceInputMutation(
            trackIndex,
            sourceNodeId,
            std::nullopt,
            std::move(normalizedChannels),
            origin,
            "Add device input");
    }

    bool TimelineFacadeImpl::setDeviceInputChannels(
            int32_t trackIndex,
            int32_t sourceNodeId,
            const std::vector<uint32_t>& channelIndices,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        auto source = targetTrack
            ? targetTrack->getSourceNode(sourceNodeId)
            : nullptr;
        auto deviceInput =
            std::dynamic_pointer_cast<DeviceInputSourceNode>(source);
        if (!deviceInput)
            return false;
        return performDeviceInputMutation(
            trackIndex,
            sourceNodeId,
            deviceInput->getInputChannels(),
            channelIndices,
            origin,
            "Change device input routing");
    }

    bool TimelineFacadeImpl::removeDeviceInputFromTrack(
            int32_t trackIndex,
            int32_t sourceNodeId,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        auto source = targetTrack
            ? targetTrack->getSourceNode(sourceNodeId)
            : nullptr;
        auto deviceInput =
            std::dynamic_pointer_cast<DeviceInputSourceNode>(source);
        if (!deviceInput)
            return false;
        return performDeviceInputMutation(
            trackIndex,
            sourceNodeId,
            deviceInput->getInputChannels(),
            std::nullopt,
            origin,
            "Remove device input");
    }

    bool TimelineFacadeImpl::applyGraphConnectionState(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& desired,
            bool present,
            std::string& error) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        auto* track = resolveSequencerTrack(trackIndex);
        auto* graph = track
            ? dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(&track->graph())
            : nullptr;
        if (!graph) {
            error = "Track graph is not a full DAG graph";
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

    bool TimelineFacadeImpl::performGraphConnectionMutation(
            int32_t trackIndex,
            uapmd_graph::AudioPluginGraphConnection connection,
            bool present,
            std::string& error,
            ProjectMutationOrigin origin) {
                auto* timelineTrack = resolveTrack(trackIndex);
        if (!timelineTrack) {
            error = "Track not found";
            return false;
        }
        auto trackReferenceId = timelineTrack->referenceId();
        auto apply = [this](
                         std::string_view persistentTrackId,
                         const uapmd_graph::AudioPluginGraphConnection& value,
                         bool desiredPresence,
                         std::string& applyError) {
            return applyGraphConnectionState(
                persistentTrackId,
                value,
                desiredPresence,
                applyError);
        };
        if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote)
            return apply(trackReferenceId, connection, present, error);

        auto operation = std::make_shared<GraphConnectionUndoOperation>(
            present,
            std::move(trackReferenceId),
            std::move(connection),
            std::move(apply));
        auto result = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.perform(
            std::move(operation),
            origin,
            [result](ProjectUndoResult completed) {
                *result = std::move(completed);
            });
        if (!result->has_value()) {
            error = "The undo engine did not complete the graph mutation inline";
            return false;
        }
        if (!result->value().succeeded()) {
            error = result->value().error;
            return false;
        }
        return true;
    }

    bool TimelineFacadeImpl::connectTrackGraph(
            int32_t trackIndex,
            const uapmd_graph::AudioPluginGraphConnection& connection,
            std::string& error,
            ProjectMutationOrigin origin) {
                return performGraphConnectionMutation(
            trackIndex,
            connection,
            true,
            error,
            origin);
    }

    bool TimelineFacadeImpl::disconnectTrackGraphConnection(
            int32_t trackIndex,
            int64_t connectionId,
            std::string& error,
            ProjectMutationOrigin origin) {
                auto* track = resolveSequencerTrack(trackIndex);
        auto* graph = track
            ? dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(&track->graph())
            : nullptr;
        if (!graph) {
            error = "Track graph is not a full DAG graph";
            return false;
        }
        const auto connections = graph->connections();
        auto connection = std::find_if(
            connections.begin(),
            connections.end(),
            [connectionId](const auto& candidate) {
                return candidate.id == connectionId;
            });
        if (connection == connections.end()) {
            error = "Connection not found";
            return false;
        }
        return performGraphConnectionMutation(
            trackIndex,
            *connection,
            false,
            error,
            origin);
    }

    bool TimelineFacadeImpl::setTrackGain(
            int32_t trackIndex,
            double gain,
            ProjectMutationOrigin origin) {
                return executeTrackProperty<TrackGainProperty>(trackIndex, gain, origin);
    }

    bool TimelineFacadeImpl::setTrackMuted(
            int32_t trackIndex,
            bool muted,
            ProjectMutationOrigin origin) {
                return executeTrackProperty<TrackMutedProperty>(trackIndex, muted, origin);
    }

    bool TimelineFacadeImpl::setTrackSolo(
            int32_t trackIndex,
            bool solo,
            ProjectMutationOrigin origin) {
                return executeTrackProperty<TrackSoloProperty>(trackIndex, solo, origin);
    }

    bool TimelineFacadeImpl::setTrackBypassed(
            int32_t trackIndex,
            bool bypassed,
            ProjectMutationOrigin origin) {
                return executeTrackProperty<TrackBypassedProperty>(trackIndex, bypassed, origin);
    }

    bool TimelineFacadeImpl::setTrackFreezePolicyEnabled(
            int32_t trackIndex,
            bool enabled,
            ProjectMutationOrigin origin) {
                // The master track has no freeze policy.
        if (trackIndex == kMasterTrackIndex)
            return false;
        return executeTrackProperty<TrackFreezePolicyProperty>(trackIndex, enabled, origin);
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
