#include "TimelineFacadeImpl.hpp"

// Plug-in state, presets, parameters and instance lifecycle -- every path that
// has to survive a plug-in being destroyed and recreated under a new runtime id.

namespace uapmd {
    PluginMutationScopePtr TimelineFacadeImpl::beginPluginMutationScope(bool includeParameters) {
        return std::make_shared<PluginMutationScope>(
            includeParameters ? &plugin_parameter_mutation_depth_ : nullptr,
            plugin_state_mutation_depth_);
    }

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
            PluginMutationScopePtr scope,
            ProjectUndoCompletion completion) {
                refreshPluginParameterCache(instanceId);
        if (error.empty())
            if (auto* restored = engine_.getPluginInstance(instanceId))
                plugin_state_values_[instanceId] = restored->saveStateSync();
        scope.reset();
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
        auto scope = beginPluginMutationScope(true);
        instance->loadState(
            std::move(state),
            StateContextType::Project,
            false,
            nullptr,
            [this,
             trackReferenceId = std::move(trackReferenceId),
             nodeId = std::move(nodeId),
             scope = std::move(scope),
             completion = std::move(completion)](
                std::string error, void*) mutable {
                    dispatchToModelThread(
                    [this,
                     trackReferenceId = std::move(trackReferenceId),
                     nodeId = std::move(nodeId),
                     scope = std::move(scope),
                     error = std::move(error),
                     completion = std::move(completion)]() mutable {
                         finishPluginMutation(
                            resolvePluginInstanceId(trackReferenceId, nodeId),
                            trackReferenceId,
                            std::move(error),
                            std::move(scope),
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
        auto scope = beginPluginMutationScope(true);
        instance->loadPreset(
            presetIndex,
            [this,
             instanceId,
             address = std::move(address),
             scope = std::move(scope),
             completion = std::move(completion)](
                std::string error, void*) mutable {
                    dispatchToModelThread(
                    [this,
                     instanceId,
                     address = std::move(address),
                     scope = std::move(scope),
                     error = std::move(error),
                     completion = std::move(completion)]() mutable {
                         finishPluginMutation(
                            instanceId,
                            address.trackReferenceId,
                            std::move(error),
                            std::move(scope),
                            std::move(completion));
                    });
            });
    }

    void TimelineFacadeImpl::restorePluginInstance(
            std::string_view trackReferenceId,
            std::string_view format,
            std::string_view pluginId,
            std::string_view nodeId,
            uint8_t group,
            const std::vector<uint8_t>& state,
            std::function<void(int32_t, std::string)> finished) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        if (trackIndex < 0 && trackIndex != kMasterTrackIndex) {
            finished(-1, "The plug-in's track no longer exists");
            return;
        }
        auto formatCopy = std::string(format);
        auto pluginIdCopy = std::string(pluginId);
        // Restoring an instance can produce parameter/state notifications both
        // while the host creates it and while its project state is loaded.
        // Keep the mutation scope alive across the whole asynchronous restore;
        // otherwise those notifications are mistaken for a new user edit and
        // replace the redo entry for the plug-in removal.
        auto scope = beginPluginMutationScope(true);
        auto finish = [scope = std::move(scope), finished = std::move(finished)](
                          int32_t instanceId,
                          std::string error) mutable {
            if (finished)
                finished(instanceId, std::move(error));
            // A host may enqueue its final parameter notifications while
            // completing loadState(), including after invoking the completion
            // callback. Use two event-loop turns: the first keeps the scope
            // alive while callbacks enqueue their follow-up notifications;
            // the second releases it after those notifications are handled.
            remidy::EventLoop::enqueueTaskOnMainThread(
                [scope = std::move(scope)]() mutable {
                    remidy::EventLoop::enqueueTaskOnMainThread(
                        [scope = std::move(scope)]() mutable {});
                });
        };
        engine_.addPluginToTrack(
            trackIndex,
            formatCopy,
            pluginIdCopy,
            [this, group, state, finish = std::move(finish)](
                int32_t newInstanceId,
                int32_t,
                std::string addError) mutable {
                if (!addError.empty() || newInstanceId < 0) {
                    finish(newInstanceId, std::move(addError));
                    return;
                }
                if (group != 0xFF && !engine_.setInstanceGroup(newInstanceId, group)) {
                    engine_.removePluginInstance(newInstanceId);
                    finish(-1, "Could not restore the plug-in UMP group");
                    return;
                }
                auto* restored = engine_.getPluginInstance(newInstanceId);
                if (!restored) {
                    finish(-1, "Restored plug-in instance is unavailable");
                    return;
                }
                if (state.empty()) {
                    finish(newInstanceId, {});
                    return;
                }
                restored->loadState(
                    state,
                    StateContextType::Project,
                    false,
                    nullptr,
                    [newInstanceId, finish = std::move(finish)](
                        std::string stateError, void*) mutable {
                        finish(
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
             group = engine_.getInstanceGroup(instanceId),
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
                     group,
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
                        undo_engine_.recordPerformed(
                            std::make_shared<PluginInstanceUndoOperation>(
                                isAddition,
                                instanceId,
                                std::move(target.trackReferenceId),
                                std::move(format),
                                std::move(pluginId),
                                std::move(target.nodeId),
                                group,
                                std::move(state),
                                [this](
                                    std::string_view trackReferenceId,
                                    std::string_view format,
                                    std::string_view pluginId,
                                    std::string_view nodeId,
                                    uint8_t group,
                                    const std::vector<uint8_t>& state,
                                    std::function<void(int32_t, std::string)> finished) {
                                    restorePluginInstance(
                                        trackReferenceId, format, pluginId,
                                        nodeId, group, state, std::move(finished));
                                },
                                [this](
                                    int32_t currentInstanceId,
                                    std::function<void(std::string)> finished) {
                                    removePluginInstanceById(
                                        currentInstanceId, std::move(finished));
                                }),
                            origin,
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
                        undo_engine_.perform(
                            std::make_shared<PluginStateUndoOperation>(
                                target.trackReferenceId,
                                target.nodeId,
                                std::move(before),
                                std::move(*after),
                                pluginStateApplier()),
                            origin,
                            std::move(completion));
                    });
            });
    }

    PluginStateUndoOperation::Apply TimelineFacadeImpl::pluginStateApplier() {
        return [this](
                   std::string_view trackReferenceId,
                   std::string_view nodeId,
                   const std::vector<uint8_t>& value,
                   ProjectUndoCompletion applied) {
            applyPluginState(
                std::string(trackReferenceId),
                std::string(nodeId),
                value,
                std::move(applied));
        };
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
                                undo_engine_.recordPerformed(
                                    std::make_shared<PluginStateUndoOperation>(
                                        target.trackReferenceId,
                                        target.nodeId,
                                        std::move(before),
                                        std::vector<uint8_t>{},
                                        pluginStateApplier(),
                                        "Load plug-in preset",
                                        [this, target, presetIndex](
                                            ProjectUndoCompletion redone) {
                                            applyPluginPreset(
                                                target, presetIndex, std::move(redone));
                                        }),
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
            double value,
            bool historySuppressed) {
                historySuppressed = historySuppressed
            || plugin_parameter_mutation_depth_.load(std::memory_order_acquire) != 0;
        if (!remidy::EventLoop::runningOnMainThread()) {
            dispatchToModelThread([this,
                                   instanceId,
                                   parameterIndex,
                                   value,
                                   historySuppressed] {
                                       onPluginParameterChanged(
                    instanceId,
                    parameterIndex,
                    value,
                    historySuppressed);
            });
            return;
        }
        const auto index = static_cast<int32_t>(parameterIndex);
        auto& values = plugin_parameter_values_[instanceId];
        const auto previous = values.find(index);
        const auto before = previous == values.end() ? value : previous->second;
        values[index] = value;
        if (previous == values.end() || historySuppressed)
            return;
        recordExternalPluginParameterChange(instanceId, index, before, value);
    }

    void TimelineFacadeImpl::onPluginStateChanged(
            int32_t instanceId,
            bool historySuppressed) {
                historySuppressed = historySuppressed
            || plugin_state_mutation_depth_.load(std::memory_order_acquire) != 0;
        if (!remidy::EventLoop::runningOnMainThread()) {
            dispatchToModelThread([this, instanceId, historySuppressed] {
                onPluginStateChanged(instanceId, historySuppressed);
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
            [this, instanceId, historySuppressed](
                std::vector<uint8_t> state,
                std::string error,
                void*) mutable {
                    dispatchToModelThread(
                    [this,
                     instanceId,
                     historySuppressed,
                     state = std::move(state),
                     error = std::move(error)]() mutable {
                         pending_plugin_state_captures_.erase(instanceId);
                        if (!error.empty())
                            return;
                        auto previous = plugin_state_values_.find(instanceId);
                        const bool changed = previous != plugin_state_values_.end()
                            && previous->second != state;
                        auto before = previous == plugin_state_values_.end()
                            ? std::vector<uint8_t>{}
                            : previous->second;
                        plugin_state_values_[instanceId] = state;
                        if (!changed || historySuppressed)
                            return;
                        auto target = pluginTargetForInstance(instanceId);
                        if (!target)
                            return;
                        auto apply = [this] (
                            std::string_view trackReferenceId,
                            std::string_view nodeId,
                            const std::vector<uint8_t>& value,
                            ProjectUndoCompletion completion) {
                            applyPluginState(
                                std::string(trackReferenceId),
                                std::string(nodeId),
                                value,
                                std::move(completion));
                        };
                        auto operation =
                            std::make_shared<PluginStateUndoOperation>(
                                target->trackReferenceId,
                                target->nodeId,
                                std::move(before),
                                std::move(state),
                                std::move(apply),
                                "Change plug-in state");
                        undo_engine_.recordPerformed(
                            std::move(operation),
                            ProjectMutationOrigin::User);
                        emitTrackChanged(
                            target->trackReferenceId,
                            "plugin-state-changed");
                        notifyTimelineChanged();
                    });
            });
    }

    void TimelineFacadeImpl::recordExternalPluginParameterChange(
            int32_t instanceId,
            int32_t parameterIndex,
            double before,
            double after) {
                auto plugin = pluginTargetForInstance(instanceId);
        if (!plugin || before == after)
            return;
        const PluginParameterAddress address{
            .plugin = *plugin,
            .parameterIndex = parameterIndex
        };
        command_manager_.recordExecuted(
            std::make_shared<PropertyCommand<PluginParameterProperty>>(
                *this, address, after),
            std::make_shared<PropertyCommand<PluginParameterProperty>>(
                *this, address, before),
            ProjectMutationOrigin::User);
        emitTrackChanged(
            plugin->trackReferenceId,
            std::format("plugin-parameter-{}-changed", parameterIndex));
        notifyTimelineChanged();
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
        plugin_parameter_mutation_depth_.fetch_add(1, std::memory_order_acq_rel);
        engine_.setParameterValue(currentInstanceId, parameterIndex, value);
        plugin_parameter_mutation_depth_.fetch_sub(1, std::memory_order_acq_rel);
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
        // Some hosts expose parameter changes before (or without) a
        // complete metadata list. Refresh indices already observed by the
        // history bridge as well, so a state restore followed by a late
        // notification cannot be mistaken for a new user edit.
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
