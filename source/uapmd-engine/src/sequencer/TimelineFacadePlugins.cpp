#include "TimelineFacadeImpl.hpp"

// Plug-in state, presets, parameters and instance lifecycle -- every path that
// has to survive a plug-in being destroyed and recreated under a new runtime id.

namespace uapmd {
    ProjectUndoCompletion TimelineFacadeImpl::trackPendingPluginMutation(ProjectUndoCompletion completion) {
        pending_plugin_mutations_.fetch_add(1, std::memory_order_acq_rel);
        return [this, completion = std::move(completion)](
                   ProjectUndoResult result) mutable {
                       pending_plugin_mutations_.fetch_sub(1, std::memory_order_acq_rel);
            if (completion)
                completion(std::move(result));
        };
    }

    void TimelineFacadeImpl::finishPluginMutation(
            int32_t instanceId,
            std::string_view trackReferenceId,
            std::string error,
            ProjectUndoCompletion completion) {
                refreshPluginParameterCache(instanceId);
        if (error.empty())
            if (auto* restored = engine_.getPluginInstance(instanceId))
                plugin_state_values_[instanceId] = restored->saveStateSync();
        if (!error.empty()) {
            if (completion)
                completion(ProjectUndoResult::failure(std::move(error)));
            return;
        }
        emitTrackChanged(trackReferenceId, "plugin-state-changed");
        notifyTimelineChanged();
        if (completion)
            completion(ProjectUndoResult::success());
    }

    void TimelineFacadeImpl::applyPluginState(
            std::string trackReferenceId,
            std::string nodeId,
            std::vector<uint8_t> state,
            ProjectUndoCompletion completion) {
                const auto instanceId = resolvePluginInstanceId(trackReferenceId, nodeId);
        auto* instance = engine_.getPluginInstance(instanceId);
        if (!instance) {
            if (completion)
                completion(ProjectUndoResult::failure(
                    "The plug-in instance no longer exists"));
            return;
        }
        instance->loadState(
            std::move(state),
            StateContextType::Project,
            false,
            nullptr,
            [this,
             trackReferenceId = std::move(trackReferenceId),
             nodeId = std::move(nodeId),
             completion = std::move(completion)](
                std::string error, void*) mutable {
                    dispatchToModelThread(
                    [this,
                     trackReferenceId = std::move(trackReferenceId),
                     nodeId = std::move(nodeId),
                     error = std::move(error),
                     completion = std::move(completion)]() mutable {
                         finishPluginMutation(
                            resolvePluginInstanceId(trackReferenceId, nodeId),
                            trackReferenceId,
                            std::move(error),
                            std::move(completion));
                    });
            });
    }

    void TimelineFacadeImpl::applyPluginPreset(
            PluginAddress address,
            int32_t presetIndex,
            ProjectUndoCompletion completion) {
                const auto instanceId = resolvePluginInstanceId(
            address.trackReferenceId,
            address.nodeId);
        auto* instance = engine_.getPluginInstance(instanceId);
        if (!instance) {
            if (completion)
                completion(ProjectUndoResult::failure(
                    "The plug-in instance no longer exists"));
            return;
        }
        instance->loadPreset(
            presetIndex,
            [this,
             instanceId,
             address = std::move(address),
             completion = std::move(completion)](
                std::string error, void*) mutable {
                    dispatchToModelThread(
                    [this,
                     instanceId,
                     address = std::move(address),
                     error = std::move(error),
                     completion = std::move(completion)]() mutable {
                         finishPluginMutation(
                            instanceId,
                            address.trackReferenceId,
                            std::move(error),
                            std::move(completion));
                    });
            });
    }

    void TimelineFacadeImpl::restorePluginInstance(
            std::string_view trackReferenceId,
            std::string_view format,
            std::string_view pluginId,
            std::string_view nodeId,
            bool bypassed,
            uint8_t group,
            const std::vector<uint8_t>& state,
            const std::vector<uapmd_graph::AudioPluginGraphConnection>& connections,
            std::function<void(int32_t, std::string)> finished) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        if (trackIndex < 0 && trackIndex != kMasterTrackIndex) {
            finished(-1, "The plug-in's track no longer exists");
            return;
        }
        auto formatCopy = std::string(format);
        auto pluginIdCopy = std::string(pluginId);
        auto trackReferenceIdCopy = std::string(trackReferenceId);
        auto finish = [finished = std::move(finished)](
                          int32_t instanceId,
                          std::string error) mutable {
            if (finished)
                finished(instanceId, std::move(error));
        };
        auto restoreConnections =
            [this,
             trackReferenceId = std::move(trackReferenceIdCopy),
             connections,
             finish = std::move(finish)](
                int32_t instanceId,
                std::string error) mutable {
                if (!error.empty() || instanceId < 0) {
                    finish(instanceId, std::move(error));
                    return;
                }
                for (const auto& connection : connections) {
                    std::string connectionError;
                    if (applyGraphConnectionState(
                            trackReferenceId,
                            connection,
                            true,
                            connectionError))
                        continue;
                    engine_.removePluginInstance(instanceId);
                    finish(
                        -1,
                        connectionError.empty()
                            ? "Could not restore the plug-in graph connections"
                            : std::move(connectionError));
                    return;
                }
                finish(instanceId, {});
            };
        engine_.addPluginToTrack(
            trackIndex,
            formatCopy,
            pluginIdCopy,
            [this, bypassed, group, state, restoreConnections = std::move(restoreConnections)](
                int32_t newInstanceId,
                int32_t,
                std::string addError) mutable {
                if (!addError.empty() || newInstanceId < 0) {
                    restoreConnections(newInstanceId, std::move(addError));
                    return;
                }
                if (group != 0xFF && !engine_.setInstanceGroup(newInstanceId, group)) {
                    engine_.removePluginInstance(newInstanceId);
                    restoreConnections(-1, "Could not restore the plug-in UMP group");
                    return;
                }
                auto* restored = engine_.getPluginInstance(newInstanceId);
                if (!restored) {
                    restoreConnections(-1, "Restored plug-in instance is unavailable");
                    return;
                }
                restored->bypassed(bypassed);
                if (state.empty()) {
                    restoreConnections(newInstanceId, {});
                    return;
                }
                restored->loadState(
                    state,
                    StateContextType::Project,
                    false,
                    nullptr,
                    [newInstanceId, restoreConnections = std::move(restoreConnections)](
                        std::string stateError, void*) mutable {
                        restoreConnections(
                            stateError.empty() ? newInstanceId : -1,
                            std::move(stateError));
                    });
            },
            std::string(nodeId));
    }

    void TimelineFacadeImpl::removePluginInstanceById(
            int32_t instanceId,
            std::function<void(std::string)> finished) {        if (!engine_.removePluginInstance(instanceId)) {
            finished("Could not remove the plug-in instance");
            return;
        }
        finished({});
    }

    void TimelineFacadeImpl::recordPluginInstanceLifecycle(
            int32_t instanceId,
            bool isAddition,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) {
                auto target = pluginTargetForInstance(instanceId);
        auto* instance = engine_.getPluginInstance(instanceId);
        const auto trackIndex = engine_.findTrackIndexForInstance(instanceId);
        if (!target || !instance
            || (trackIndex < 0 && trackIndex != kMasterTrackIndex)) {
            if (completion)
                completion(ProjectUndoResult::failure(
                    "The plug-in instance does not exist"));
            return;
        }
        std::vector<uapmd_graph::AudioPluginGraphConnection> connections;
        if (auto* track = resolveSequencerTrack(trackIndex))
            if (auto* graph = dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(
                    &track->graph()))
                for (auto connection : graph->connections()) {
                    auto normalizeEndpoint = [track](auto& endpoint) {
                        if (endpoint.type
                                != uapmd_graph::AudioPluginGraphEndpointType::Plugin
                            || !endpoint.node_id.empty())
                            return;
                        if (auto* node = track->graph().getPluginNode(
                                endpoint.instance_id))
                            endpoint.node_id = node->nodeId();
                    };
                    normalizeEndpoint(connection.source);
                    normalizeEndpoint(connection.target);
                    const auto touchesInstance =
                        (connection.source.type
                                == uapmd_graph::AudioPluginGraphEndpointType::Plugin
                            && connection.source.node_id == target->nodeId)
                        || (connection.target.type
                                == uapmd_graph::AudioPluginGraphEndpointType::Plugin
                            && connection.target.node_id == target->nodeId);
                    if (touchesInstance)
                        connections.push_back(std::move(connection));
                }
        auto finish = trackPendingPluginMutation(std::move(completion));
        instance->requestState(
            StateContextType::Project,
            false,
            nullptr,
            [this,
             origin,
             instanceId,
             isAddition,
             target = std::move(*target),
             format = instance->formatName(),
             pluginId = instance->pluginId(),
             bypassed = instance->bypassed(),
             group = engine_.getInstanceGroup(instanceId),
             connections = std::move(connections),
             completion = std::move(finish)](
                std::vector<uint8_t> state,
                std::string error,
                void*) mutable {
                    dispatchToModelThread(
                    [this,
                     origin,
                     instanceId,
                     isAddition,
                     target = std::move(target),
                     format = std::move(format),
                     pluginId = std::move(pluginId),
                     bypassed,
                     group,
                     connections = std::move(connections),
                     state = std::move(state),
                     error = std::move(error),
                     completion = std::move(completion)]() mutable {if (!error.empty()) {
                            if (completion)
                                completion(ProjectUndoResult::failure(std::move(error)));
                            return;
                        }
                        if (!isAddition && !engine_.removePluginInstance(instanceId)) {
                            if (completion)
                                completion(ProjectUndoResult::failure(
                                    "Could not remove the plug-in instance"));
                            return;
                        }
                        // The instance is already added, or already removed;
                        // both directions are recorded as the pair that moves
                        // between "this plug-in, configured like so" and "no
                        // plug-in at this node".
                        timeline_detail::PluginInstanceSnapshot snapshot{
                            .format = std::move(format),
                            .pluginId = std::move(pluginId),
                            .bypassed = bypassed,
                            .group = group,
                            .state = std::move(state),
                            .connections = std::move(connections)
                        };
                        const PluginAddress address{
                            std::move(target.trackReferenceId), std::move(target.nodeId)};
                        const std::string label =
                            isAddition ? "Add plug-in" : "Remove plug-in";
                        using Command = timeline_detail::PluginPresenceCommand;
                        auto present = std::make_shared<Command>(
                            *this, address, std::move(snapshot), label);
                        auto absent = std::make_shared<Command>(
                            *this, address, std::nullopt, label);
                        if (isAddition)
                            recordPluginChange(
                                std::move(present), std::move(absent), origin,
                                std::move(completion));
                        else
                            recordPluginChange(
                                std::move(absent), std::move(present), origin,
                                std::move(completion));
                    });
            });
    }

    void TimelineFacadeImpl::setPluginState(
            int32_t instanceId,
            std::vector<uint8_t> state,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) {
                auto target = pluginTargetForInstance(instanceId);
        auto* instance = engine_.getPluginInstance(instanceId);
        if (!target || !instance) {
            if (completion)
                completion(ProjectUndoResult::failure(
                    "The plug-in instance does not exist"));
            return;
        }
        if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote) {
            applyPluginState(
                std::move(target->trackReferenceId),
                std::move(target->nodeId),
                std::move(state),
                std::move(completion));
            return;
        }

        auto after = std::make_shared<std::vector<uint8_t>>(std::move(state));
        instance->requestState(
            StateContextType::Project,
            false,
            nullptr,
            [this,
             origin,
             after,
             target = std::move(*target),
             completion = std::move(completion)](
                std::vector<uint8_t> before,
                std::string error,
                void*) mutable {
                    dispatchToModelThread(
                    [this,
                     origin,
                     before = std::move(before),
                     error = std::move(error),
                     after,
                     target = std::move(target),
                     completion = std::move(completion)]() mutable {if (!error.empty()) {
                            if (completion)
                                completion(ProjectUndoResult::failure(std::move(error)));
                            return;
                        }
                        // The state it is about to replace is now in hand, so
                        // the write is applied here and recorded as the pair
                        // that moves between the two blobs.
                        using Command = timeline_detail::PluginStateCommand;
                        const PluginAddress address{
                            target.trackReferenceId, target.nodeId};
                        auto forward = std::make_shared<Command>(
                            *this, address, *after, "Load plug-in state");
                        auto revert = std::make_shared<Command>(
                            *this, address, std::move(before), "Load plug-in state");
                        writePluginState(
                            address,
                            *after,
                            [this, origin, forward = std::move(forward),
                             revert = std::move(revert),
                             completion = std::move(completion)](
                                ProjectCommandResult applied) mutable {
                                if (!applied.succeeded()) {
                                    if (completion)
                                        completion(std::move(applied));
                                    return;
                                }
                                recordPluginChange(
                                    std::move(forward), std::move(revert), origin,
                                    std::move(completion));
                            });
                    });
            });
    }

    void TimelineFacadeImpl::writePluginPresence(
            const PluginAddress& address,
            const std::optional<timeline_detail::PluginInstanceSnapshot>& snapshot,
            ProjectCommandCompletion completion) {
        if (!snapshot) {
            const auto currentInstanceId =
                resolvePluginInstanceId(address.trackReferenceId, address.nodeId);
            removePluginInstanceById(
                currentInstanceId,
                [completion = std::move(completion)](std::string error) mutable {
                    if (!completion)
                        return;
                    completion(
                        error.empty()
                            ? ProjectCommandResult::success()
                            : ProjectCommandResult::failure(std::move(error)));
                });
            return;
        }
        restorePluginInstance(
            address.trackReferenceId,
            snapshot->format,
            snapshot->pluginId,
            address.nodeId,
            snapshot->bypassed,
            snapshot->group,
            snapshot->state,
            snapshot->connections,
            [completion = std::move(completion)](
                int32_t restoredInstanceId, std::string error) mutable {
                if (!completion)
                    return;
                if (restoredInstanceId >= 0 && error.empty()) {
                    completion(ProjectCommandResult::success());
                    return;
                }
                completion(ProjectCommandResult::failure(
                    error.empty() ? "Could not restore the plug-in" : std::move(error)));
            });
    }

    void TimelineFacadeImpl::loadPluginPreset(
            int32_t instanceId,
            int32_t presetIndex,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) {
                auto target = pluginTargetForInstance(instanceId);
        auto* instance = engine_.getPluginInstance(instanceId);
        if (!target || !instance || presetIndex < 0) {
            if (completion)
                completion(ProjectUndoResult::failure(
                    "The plug-in preset target is invalid"));
            return;
        }

        if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote) {
            applyPluginPreset(std::move(*target), presetIndex, std::move(completion));
            return;
        }

        auto finish = trackPendingPluginMutation(std::move(completion));
        instance->requestState(
            StateContextType::Project,
            false,
            nullptr,
            [this,
             presetIndex,
             origin,
             target = std::move(*target),
             completion = std::move(finish)](
                std::vector<uint8_t> before,
                std::string error,
                void*) mutable {
                    dispatchToModelThread(
                    [this,
                     presetIndex,
                     origin,
                     target = std::move(target),
                     before = std::move(before),
                     error = std::move(error),
                     completion = std::move(completion)]() mutable {if (!error.empty()) {
                            if (completion)
                                completion(ProjectUndoResult::failure(std::move(error)));
                            return;
                        }
                        applyPluginPreset(
                            target,
                            presetIndex,
                            [this,
                             presetIndex,
                             origin,
                             target,
                             before = std::move(before),
                             completion = std::move(completion)](
                                ProjectUndoResult result) mutable {if (!result.succeeded()) {
                                    if (completion)
                                        completion(std::move(result));
                                    return;
                                }
                                // The preset is already loaded; history
                                // holds the state it replaced, and redo
                                // reloads the same preset.
                                // Redo reloads the same preset rather than
                                // writing back the bytes it produced, so the
                                // two directions are different command types.
                                const PluginAddress address{
                                    target.trackReferenceId, target.nodeId};
                                recordPluginChange(
                                    std::make_shared<timeline_detail::PluginPresetCommand>(
                                        *this, address, presetIndex, "Load plug-in preset"),
                                    std::make_shared<timeline_detail::PluginStateCommand>(
                                        *this, address, std::move(before),
                                        "Load plug-in preset"),
                                    origin,
                                    std::move(completion));
                            });
                    });
            });
    }

    void TimelineFacadeImpl::recordPluginInstanceAddition(
            int32_t instanceId,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) {
                recordPluginInstanceLifecycle(instanceId, true, origin, std::move(completion));
    }

    void TimelineFacadeImpl::removePluginInstance(
            int32_t instanceId,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) {
                if (origin != ProjectMutationOrigin::User
            && origin != ProjectMutationOrigin::Remote) {
            // Nothing to capture, so the instance simply goes away.
            if (!engine_.getPluginInstance(instanceId)) {
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "The plug-in instance does not exist"));
                return;
            }
            const auto removed = engine_.removePluginInstance(instanceId);
            if (completion)
                completion(removed
                    ? ProjectUndoResult::success()
                    : ProjectUndoResult::failure(
                        "Could not remove the plug-in instance"));
            return;
        }
        recordPluginInstanceLifecycle(instanceId, false, origin, std::move(completion));
    }

    bool TimelineFacadeImpl::hasPendingPluginMutations() const {
        return pending_plugin_mutations_.load(std::memory_order_acquire) != 0;
    }

    void TimelineFacadeImpl::onPluginParameterChanged(
            int32_t instanceId,
            uint32_t parameterIndex,
            double value) {
        if (!remidy::EventLoop::runningOnMainThread()) {
            dispatchToModelThread([this,
                                   instanceId,
                                   parameterIndex,
                                   value] {
                                       onPluginParameterChanged(
                    instanceId,
                    parameterIndex,
                    value);
            });
            return;
        }
        const auto index = static_cast<int32_t>(parameterIndex);
        plugin_parameter_values_[instanceId][index] = value;
    }

    void TimelineFacadeImpl::onPluginStateChanged(int32_t instanceId) {
        if (!remidy::EventLoop::runningOnMainThread()) {
            dispatchToModelThread([this, instanceId] {
                onPluginStateChanged(instanceId);
            });
            return;
        }
        if (pending_plugin_state_captures_.contains(instanceId))
            return;
        auto* instance = engine_.getPluginInstance(instanceId);
        if (!instance)
            return;
        pending_plugin_state_captures_.insert(instanceId);
        instance->requestState(
            StateContextType::Project,
            false,
            nullptr,
            [this, instanceId](
                std::vector<uint8_t> state,
                std::string error,
                void*) mutable {
                    dispatchToModelThread(
                    [this,
                     instanceId,
                     state = std::move(state),
                     error = std::move(error)]() mutable {
                         pending_plugin_state_captures_.erase(instanceId);
                        if (!error.empty())
                            return;
                        plugin_state_values_[instanceId] = state;
                    });
            });
    }

    bool TimelineFacadeImpl::applyPluginParameterValue(
            std::string_view persistentTrackId,
            std::string_view persistentNodeId,
            int32_t parameterIndex,
            double value) {
                const auto currentInstanceId = resolvePluginInstanceId(
            persistentTrackId,
            persistentNodeId);
        auto* currentInstance = engine_.getPluginInstance(currentInstanceId);
        if (!currentInstance
            || engine_.frozenTrackManager().isInstanceBusy(currentInstanceId))
            return false;
        engine_.setParameterValue(currentInstanceId, parameterIndex, value);
        plugin_parameter_values_[currentInstanceId][parameterIndex] = value;
        emitTrackChanged(persistentTrackId, std::format(
            "plugin-parameter-{}-changed", parameterIndex));
        notifyTimelineChanged();
        return true;
    }

    void TimelineFacadeImpl::refreshPluginParameterCache(int32_t instanceId) {
        auto* instance = engine_.getPluginInstance(instanceId);
        if (!instance)
            return;
        auto& values = plugin_parameter_values_[instanceId];
        for (const auto& parameter : instance->parameterMetadataList())
            values[static_cast<int32_t>(parameter.index)] =
                instance->getParameterValue(static_cast<int32_t>(parameter.index));
        // Some hosts expose parameter changes before (or without) a complete
        // metadata list. Refresh indices already observed by the timeline as
        // well so subsequent reads use the restored values.
        for (auto& [index, value] : values)
            value = instance->getParameterValue(index);
    }

    std::optional<TimelineFacadeImpl::PluginTarget> TimelineFacadeImpl::pluginTargetForInstance(
            int32_t instanceId) {
                const auto trackIndex = engine_.findTrackIndexForInstance(instanceId);
        auto* timelineTrack = resolveTrack(trackIndex);
        auto* sequencerTrack = resolveSequencerTrack(trackIndex);
        auto* node = sequencerTrack
            ? sequencerTrack->graph().getPluginNode(instanceId)
            : nullptr;
        if (!timelineTrack || !node || node->nodeId().empty())
            return std::nullopt;
        return PluginTarget{
            .trackReferenceId = timelineTrack->referenceId(),
            .nodeId = node->nodeId()
        };
    }

    int32_t TimelineFacadeImpl::resolvePluginInstanceId(
            std::string_view trackReferenceId,
            std::string_view nodeId) {
                auto* track = resolveSequencerTrackByReferenceId(trackReferenceId);
        if (!track)
            return -1;
        for (const auto instanceId : track->orderedInstanceIds()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node && node->nodeId() == nodeId)
                return instanceId;
        }
        return -1;
    }





    AudioPluginInstanceAPI* TimelineFacadeImpl::pluginInstance(int32_t instanceId) {
        return engine_.getPluginInstance(instanceId);
    }

    bool TimelineFacadeImpl::pluginInstanceBusy(int32_t instanceId) {
        return engine_.frozenTrackManager().isInstanceBusy(instanceId);
    }

    uint8_t TimelineFacadeImpl::pluginInstanceGroup(int32_t instanceId) {
        return engine_.getInstanceGroup(instanceId);
    }

    bool TimelineFacadeImpl::setPluginInstanceGroup(int32_t instanceId, uint8_t group) {
        return engine_.setInstanceGroup(instanceId, group);
    }

    bool TimelineFacadeImpl::applyPluginParameter(
            std::string_view trackReference,
            std::string_view nodeId,
            int32_t parameterIndex,
            double value) {
                return applyPluginParameterValue(trackReference, nodeId, parameterIndex, value);
    }

} // namespace uapmd
