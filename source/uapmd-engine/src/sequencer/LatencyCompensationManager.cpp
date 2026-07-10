#include "LatencyCompensationManagerImpl.hpp"

#include <algorithm>
#include <format>
#include <unordered_set>

#include "TrackRoutingManager.hpp"

namespace uapmd {

    void LatencyCompensationManagerImpl::OutputAlignmentDelayLine::reset() {
        write_position = 0;
        for (auto& bus : buses)
            for (auto& channel : bus)
                std::fill(channel.begin(), channel.end(), 0.0f);
    }

    void LatencyCompensationManagerImpl::OutputAlignmentDelayLine::configure(
        AudioProcessContext* ctx,
        size_t delayFrames) {
        capacity_frames = std::max<size_t>(1, delayFrames + 1);
        const uint32_t busCount = ctx ? ctx->audioOutBusCount() : 0;
        buses.assign(busCount, {});
        for (uint32_t busIndex = 0; busIndex < busCount; ++busIndex) {
            const uint32_t channelCount = ctx->outputChannelCount(busIndex);
            buses[busIndex].assign(channelCount, std::vector<float>(capacity_frames, 0.0f));
        }
        write_position = 0;
    }

    LatencyCompensationManagerImpl::LatencyCompensationManagerImpl(
        size_t& audioBufferSizeInFrames,
        std::vector<std::unique_ptr<SequencerTrack>>& tracks,
        std::unique_ptr<SequencerTrack>& masterTrack,
        SequenceProcessContext& sequence,
        std::atomic<bool>& isPlaybackActive,
        std::atomic<int64_t>& playbackPositionSamples,
        std::atomic<int64_t>& renderPlaybackPositionSamples,
        std::atomic<bool>& latencyDrainActive,
        std::atomic<int64_t>& latencyDrainRemainingSamples,
        std::atomic<bool>& resetToStartAfterLatencyDrain,
        std::function<void(const std::function<void()>&)> runMutation,
        std::function<AudioPluginInstanceAPI*(int32_t)> resolvePluginInstance,
        std::function<void()> prepareForTimingChange)
        : audio_buffer_size_in_frames_(audioBufferSizeInFrames)
        , tracks_(tracks)
        , master_track_(masterTrack)
        , sequence_(sequence)
        , is_playback_active_(isPlaybackActive)
        , playback_position_samples_(playbackPositionSamples)
        , render_playback_position_samples_(renderPlaybackPositionSamples)
        , latency_drain_active_(latencyDrainActive)
        , latency_drain_remaining_samples_(latencyDrainRemainingSamples)
        , reset_to_start_after_latency_drain_(resetToStartAfterLatencyDrain)
        , run_mutation_(std::move(runMutation))
        , resolve_plugin_instance_(std::move(resolvePluginInstance))
        , prepare_for_timing_change_(std::move(prepareForTimingChange)) {
    }

    void LatencyCompensationManagerImpl::clearPluginTimingListeners() {
        callbacks_alive_->store(false, std::memory_order_release);
        while (!timing_listener_ids_.empty())
            removePluginTimingListener(timing_listener_ids_.begin()->first);
    }

    void LatencyCompensationManagerImpl::removePluginTimingListener(int32_t instanceId) {
        const auto listenerIt = timing_listener_ids_.find(instanceId);
        const auto instanceIt = timing_listener_instances_.find(instanceId);
        if (listenerIt != timing_listener_ids_.end() &&
            instanceIt != timing_listener_instances_.end() && instanceIt->second)
            instanceIt->second->removeTimingInfoChangeListener(listenerIt->second);
        timing_listener_ids_.erase(instanceId);
        timing_listener_instances_.erase(instanceId);
        timing_snapshots_.erase(instanceId);
    }

    void LatencyCompensationManagerImpl::syncPluginTimingListeners() {
        std::unordered_set<int32_t> activeInstanceIds;
        for (const auto& track : tracks_)
            if (track)
                for (const auto instanceId : track->orderedInstanceIds())
                    activeInstanceIds.insert(instanceId);
        if (master_track_)
            for (const auto instanceId : master_track_->orderedInstanceIds())
                activeInstanceIds.insert(instanceId);

        for (auto it = timing_listener_ids_.begin(); it != timing_listener_ids_.end();) {
            const auto instanceId = it->first;
            const auto instanceIt = timing_listener_instances_.find(instanceId);
            if (activeInstanceIds.contains(instanceId) &&
                instanceIt != timing_listener_instances_.end() && instanceIt->second) {
                ++it;
                continue;
            }

            if (instanceIt != timing_listener_instances_.end() && instanceIt->second)
                instanceIt->second->removeTimingInfoChangeListener(it->second);
            timing_listener_instances_.erase(instanceId);
            timing_snapshots_.erase(instanceId);
            it = timing_listener_ids_.erase(it);
        }

        for (const auto instanceId : activeInstanceIds) {
            auto* instance = resolve_plugin_instance_ ? resolve_plugin_instance_(instanceId) : nullptr;
            if (!instance)
                continue;

            const auto listenerIt = timing_listener_ids_.find(instanceId);
            const auto registeredInstanceIt = timing_listener_instances_.find(instanceId);
            if (listenerIt != timing_listener_ids_.end() &&
                registeredInstanceIt != timing_listener_instances_.end() &&
                registeredInstanceIt->second == instance)
                continue;

            if (listenerIt != timing_listener_ids_.end() &&
                registeredInstanceIt != timing_listener_instances_.end() &&
                registeredInstanceIt->second) {
                registeredInstanceIt->second->removeTimingInfoChangeListener(listenerIt->second);
                timing_listener_ids_.erase(listenerIt);
                timing_listener_instances_.erase(registeredInstanceIt);
            }

            const auto alive = callbacks_alive_;
            const auto listenerId = instance->addTimingInfoChangeListener(
                [this, alive, instanceId](remidy::PluginTimingInfoChange change) {
                    if (!alive->load(std::memory_order_acquire))
                        return;
                    handlePluginTimingInfoChange(instanceId, change);
                });
            if (listenerId == 0)
                continue;

            timing_listener_ids_[instanceId] = listenerId;
            timing_listener_instances_[instanceId] = instance;
            timing_snapshots_[instanceId] = TimingSnapshot{
                instance->latencyInSamples(),
                instance->tailLengthInSeconds(),
            };
        }
    }

    bool LatencyCompensationManagerImpl::refreshGraphTimingInfo(int32_t instanceId) {
        for (const auto& track : tracks_) {
            if (!track || !track->graph().getPluginNode(instanceId))
                continue;
            track->graph().refreshTimingInfo();
            return true;
        }
        if (master_track_ && master_track_->graph().getPluginNode(instanceId)) {
            master_track_->graph().refreshTimingInfo();
            return true;
        }
        return false;
    }

    void LatencyCompensationManagerImpl::handlePluginTimingInfoChange(
        int32_t instanceId,
        remidy::PluginTimingInfoChange change) {
        if (!change.latency_changed && !change.tail_changed)
            return;
        if (!remidy::EventLoop::runningOnMainThread()) {
            const auto alive = callbacks_alive_;
            remidy::EventLoop::enqueueTaskOnMainThread([this, alive, instanceId, change] {
                if (alive->load(std::memory_order_acquire))
                    handlePluginTimingInfoChange(instanceId, change);
            });
            return;
        }

        auto* instance = resolve_plugin_instance_ ? resolve_plugin_instance_(instanceId) : nullptr;
        if (!instance)
            return;

        const TimingSnapshot current{
            instance->latencyInSamples(),
            instance->tailLengthInSeconds(),
        };
        const auto previousIt = timing_snapshots_.find(instanceId);
        if (previousIt != timing_snapshots_.end() &&
            previousIt->second.latency_in_samples == current.latency_in_samples &&
            previousIt->second.tail_length_in_seconds == current.tail_length_in_seconds)
            return;
        timing_snapshots_[instanceId] = current;

        run_mutation_([&]() {
            if (!refreshGraphTimingInfo(instanceId))
                return;
            if (prepare_for_timing_change_)
                prepare_for_timing_change_();
            if (track_routing_manager_)
                track_routing_manager_->rebuildRoutingCaches();
            applyLatencyCompensationTimingUpdate(
                is_playback_active_.load(std::memory_order_acquire));
        });
    }

    void LatencyCompensationManagerImpl::attachTrackRoutingManager(TrackRoutingManager& trackRoutingManager) {
        track_routing_manager_ = &trackRoutingManager;
        if (track_record_armed_.size() != tracks_.size())
            track_record_armed_.resize(tracks_.size(), 0);
        if (track_monitoring_enabled_.size() != tracks_.size())
            track_monitoring_enabled_.resize(tracks_.size(), 0);
    }

    void LatencyCompensationManagerImpl::applyStateChange() {
        if (track_routing_manager_)
            track_routing_manager_->rebuildRoutingCaches();
        reconfigureOutputAlignmentBuffers();
        resetOutputAlignmentBuffers();
        applyLatencyCompensationTimingUpdate(is_playback_active_.load(std::memory_order_acquire));
    }

    bool LatencyCompensationManagerImpl::trackRecordArmed(uapmd_track_index_t trackIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= track_record_armed_.size())
            return false;
        return track_record_armed_[static_cast<size_t>(trackIndex)] != 0;
    }

    void LatencyCompensationManagerImpl::trackRecordArmed(uapmd_track_index_t trackIndex, bool armed) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return;
        run_mutation_([&]() {
            if (static_cast<size_t>(trackIndex) >= track_record_armed_.size())
                track_record_armed_.resize(tracks_.size(), 0);
            track_record_armed_[static_cast<size_t>(trackIndex)] = armed ? 1 : 0;
            applyStateChange();
        });
    }

    bool LatencyCompensationManagerImpl::trackMonitoringEnabled(uapmd_track_index_t trackIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= track_monitoring_enabled_.size())
            return false;
        return track_monitoring_enabled_[static_cast<size_t>(trackIndex)] != 0;
    }

    void LatencyCompensationManagerImpl::trackMonitoringEnabled(uapmd_track_index_t trackIndex, bool enabled) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return;
        run_mutation_([&]() {
            if (static_cast<size_t>(trackIndex) >= track_monitoring_enabled_.size())
                track_monitoring_enabled_.resize(tracks_.size(), 0);
            track_monitoring_enabled_[static_cast<size_t>(trackIndex)] = enabled ? 1 : 0;
            applyStateChange();
        });
    }

    PlaybackCompensationMode LatencyCompensationManagerImpl::playbackCompensationMode() const {
        return playback_compensation_mode_.load(std::memory_order_acquire);
    }

    void LatencyCompensationManagerImpl::playbackCompensationMode(PlaybackCompensationMode mode) {
        run_mutation_([&]() {
            playback_compensation_mode_.store(mode, std::memory_order_release);
            applyStateChange();
        });
    }

    InputMonitoringPolicy LatencyCompensationManagerImpl::inputMonitoringPolicy() const {
        return input_monitoring_policy_.load(std::memory_order_acquire);
    }

    void LatencyCompensationManagerImpl::inputMonitoringPolicy(InputMonitoringPolicy policy) {
        run_mutation_([&]() {
            input_monitoring_policy_.store(policy, std::memory_order_release);
            applyStateChange();
        });
    }

    LatencyCompensationProjectSettings LatencyCompensationManagerImpl::projectSettings() const {
        LatencyCompensationProjectSettings settings;
        settings.implementation_id = "default";
        settings.playback_compensation_mode = playbackCompensationMode();
        settings.input_monitoring_policy = inputMonitoringPolicy();
        for (size_t i = 0; i < track_record_armed_.size(); ++i)
            if (track_record_armed_[i] != 0)
                settings.record_armed_track_indexes.push_back(static_cast<int32_t>(i));
        for (size_t i = 0; i < track_monitoring_enabled_.size(); ++i)
            if (track_monitoring_enabled_[i] != 0)
                settings.monitored_track_indexes.push_back(static_cast<int32_t>(i));
        return settings;
    }

    bool LatencyCompensationManagerImpl::applyProjectSettings(
        const LatencyCompensationProjectSettings& settings,
        std::string& error) {
        if (!settings.implementation_id.empty() && settings.implementation_id != "default") {
            error = std::format(
                "Unsupported latency compensation implementation: {}",
                settings.implementation_id);
            return false;
        }

        run_mutation_([&]() {
            playback_compensation_mode_.store(
                settings.playback_compensation_mode,
                std::memory_order_release);
            input_monitoring_policy_.store(
                settings.input_monitoring_policy,
                std::memory_order_release);
            track_record_armed_.assign(tracks_.size(), 0);
            for (auto trackIndex : settings.record_armed_track_indexes) {
                if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
                    continue;
                track_record_armed_[static_cast<size_t>(trackIndex)] = 1;
            }
            track_monitoring_enabled_.assign(tracks_.size(), 0);
            for (auto trackIndex : settings.monitored_track_indexes) {
                if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
                    continue;
                track_monitoring_enabled_[static_cast<size_t>(trackIndex)] = 1;
            }
            applyStateChange();
        });
        return true;
    }

    OutputAlignmentMonitoringPolicy LatencyCompensationManagerImpl::outputAlignmentMonitoringPolicy() const {
        return playbackCompensationMode() == PlaybackCompensationMode::LOW_LATENCY
            ? OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT
            : OutputAlignmentMonitoringPolicy::FULLY_COMPENSATED;
    }

    void LatencyCompensationManagerImpl::outputAlignmentMonitoringPolicy(OutputAlignmentMonitoringPolicy policy) {
        run_mutation_([&]() {
            playback_compensation_mode_.store(
                policy == OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT
                    ? PlaybackCompensationMode::LOW_LATENCY
                    : PlaybackCompensationMode::COMPENSATED,
                std::memory_order_release);
            output_alignment_monitoring_policy_.store(policy, std::memory_order_release);
            applyStateChange();
        });
    }

    RealtimeInfiniteTailPolicy LatencyCompensationManagerImpl::realtimeInfiniteTailPolicy() const {
        return realtime_infinite_tail_policy_.load(std::memory_order_acquire);
    }

    void LatencyCompensationManagerImpl::realtimeInfiniteTailPolicy(RealtimeInfiniteTailPolicy policy) {
        realtime_infinite_tail_policy_.store(policy, std::memory_order_release);
    }

    void LatencyCompensationManagerImpl::onTrackAdded() {
        track_record_armed_.push_back(0);
        track_monitoring_enabled_.push_back(0);
    }

    void LatencyCompensationManagerImpl::onTrackRemoved(uapmd_track_index_t trackIndex) {
        if (trackIndex >= 0 && static_cast<size_t>(trackIndex) < track_record_armed_.size())
            track_record_armed_.erase(
                track_record_armed_.begin() + static_cast<long>(trackIndex));
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= track_monitoring_enabled_.size())
            return;
        track_monitoring_enabled_.erase(
            track_monitoring_enabled_.begin() + static_cast<long>(trackIndex));
    }

    int64_t LatencyCompensationManagerImpl::alignToQuantum(int64_t samples) const {
        const auto quantum = static_cast<int64_t>(audio_buffer_size_in_frames_ > 0 ? audio_buffer_size_in_frames_ : 1);
        return ((samples + quantum - 1) / quantum) * quantum;
    }

    void LatencyCompensationManagerImpl::clearDrainState() {
        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        reset_to_start_after_latency_drain_.store(false, std::memory_order_release);
    }

    uint32_t LatencyCompensationManagerImpl::maxRenderLeadInSamples() const {
        return std::max(
            track_routing_manager_ ? track_routing_manager_->maxTrackRenderLeadInSamples() : 0,
            master_track_ ? master_track_->renderLeadInSamples() : 0);
    }

    int64_t LatencyCompensationManagerImpl::maxStopDrainInSamples() const {
        return track_routing_manager_ ? track_routing_manager_->maxStopDrainInSamples() : 0;
    }

    void LatencyCompensationManagerImpl::schedulePrerollFromAudiblePosition(int64_t samples) {
        clearDrainState();
        playback_position_samples_.store(samples, std::memory_order_release);
        render_playback_position_samples_.store(
            samples - alignToQuantum(static_cast<int64_t>(maxRenderLeadInSamples())),
            std::memory_order_release);
    }

    void LatencyCompensationManagerImpl::reconfigureOutputAlignmentBuffers() {
        output_alignment_delay_lines_.resize(tracks_.size());
        if (!track_routing_manager_)
            return;
        const size_t delayCapacityFrames =
            static_cast<size_t>(track_routing_manager_->maxOutputAlignmentHoldbackInSamples()) +
            audio_buffer_size_in_frames_;
        for (size_t i = 0; i < output_alignment_delay_lines_.size(); ++i) {
            auto* ctx = i < sequence_.tracks.size() ? sequence_.tracks[i] : nullptr;
            output_alignment_delay_lines_[i].configure(ctx, delayCapacityFrames);
        }
    }

    void LatencyCompensationManagerImpl::resetOutputAlignmentBuffers() {
        for (auto& delayLine : output_alignment_delay_lines_)
            delayLine.reset();
    }

    void LatencyCompensationManagerImpl::applyLatencyCompensationTimingUpdate(bool isPlaybackActive) {
        reconfigureOutputAlignmentBuffers();
        resetOutputAlignmentBuffers();

        const auto audiblePosition = playback_position_samples_.load(std::memory_order_acquire);
        if (isPlaybackActive) {
            schedulePrerollFromAudiblePosition(audiblePosition);
            return;
        }

        clearDrainState();
        render_playback_position_samples_.store(audiblePosition, std::memory_order_release);
    }

    void LatencyCompensationManagerImpl::setPlaybackPosition(int64_t samples, bool isPlaybackActive) {
        if (isPlaybackActive) {
            resetOutputAlignmentBuffers();
            schedulePrerollFromAudiblePosition(samples);
            return;
        }

        clearDrainState();
        playback_position_samples_.store(samples, std::memory_order_release);
        render_playback_position_samples_.store(samples, std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    void LatencyCompensationManagerImpl::startPlayback() {
        resetOutputAlignmentBuffers();
        schedulePrerollFromAudiblePosition(0);
    }

    void LatencyCompensationManagerImpl::stopPlayback() {
        resetOutputAlignmentBuffers();
        const auto tailSamples = maxStopDrainInSamples();
        if (tailSamples > 0) {
            latency_drain_active_.store(true, std::memory_order_release);
            latency_drain_remaining_samples_.store(alignToQuantum(tailSamples), std::memory_order_release);
            reset_to_start_after_latency_drain_.store(true, std::memory_order_release);
            return;
        }

        playback_position_samples_.store(0, std::memory_order_release);
        render_playback_position_samples_.store(0, std::memory_order_release);
        clearDrainState();
    }

    void LatencyCompensationManagerImpl::pausePlayback() {
        clearDrainState();
        render_playback_position_samples_.store(
            playback_position_samples_.load(std::memory_order_acquire),
            std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    void LatencyCompensationManagerImpl::resumePlayback() {
        resetOutputAlignmentBuffers();
        schedulePrerollFromAudiblePosition(playback_position_samples_.load(std::memory_order_acquire));
    }

    void LatencyCompensationManagerImpl::updateLatencyDrainState(int32_t frameCount) {
        if (!latency_drain_active_.load(std::memory_order_acquire))
            return;

        const auto remaining =
            latency_drain_remaining_samples_.fetch_sub(frameCount, std::memory_order_acq_rel) - frameCount;
        render_playback_position_samples_.fetch_add(frameCount, std::memory_order_release);
        if (remaining > 0)
            return;

        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        if (reset_to_start_after_latency_drain_.exchange(false, std::memory_order_acq_rel)) {
            playback_position_samples_.store(0, std::memory_order_release);
            render_playback_position_samples_.store(0, std::memory_order_release);
        }
    }

    void LatencyCompensationManagerImpl::applyOutputAlignment(
        uapmd_track_index_t trackIndex,
        AudioProcessContext& ctx,
        int32_t trackFrameCount) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= output_alignment_delay_lines_.size())
            return;

        auto& delayLine = output_alignment_delay_lines_[static_cast<size_t>(trackIndex)];
        if (delayLine.capacity_frames == 0)
            return;

        bool usedDelay = false;
        const size_t startWritePosition = delayLine.write_position;
        if (!track_routing_manager_)
            return;
        for (uint32_t busIndex = 0; busIndex < ctx.audioOutBusCount(); ++busIndex) {
            const uint32_t outputAlignmentDelay =
                track_routing_manager_->trackOutputAlignmentHoldbackInSamples(trackIndex, busIndex);
            if (outputAlignmentDelay == 0 || busIndex >= delayLine.buses.size())
                continue;

            auto& busStorage = delayLine.buses[busIndex];
            const size_t maxDelayFrames = delayLine.capacity_frames > 0 ? delayLine.capacity_frames - 1 : 0;
            const size_t appliedDelayFrames = std::min<size_t>(outputAlignmentDelay, maxDelayFrames);
            const uint32_t numChannels = std::min<uint32_t>(
                ctx.outputChannelCount(busIndex),
                static_cast<uint32_t>(busStorage.size()));
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                auto& delayChannel = busStorage[ch];
                float* buffer = ctx.getFloatOutBuffer(busIndex, ch);
                if (!buffer)
                    continue;
                size_t writePosition = startWritePosition;
                for (int32_t frame = 0; frame < trackFrameCount; ++frame) {
                    const float inputSample = buffer[frame];
                    delayChannel[writePosition] = inputSample;
                    const size_t readPosition =
                        (writePosition + delayLine.capacity_frames - appliedDelayFrames) %
                        delayLine.capacity_frames;
                    buffer[frame] = delayChannel[readPosition];
                    writePosition = (writePosition + 1) % delayLine.capacity_frames;
                }
            }
            usedDelay = true;
        }

        if (usedDelay)
            delayLine.write_position =
                (startWritePosition + static_cast<size_t>(trackFrameCount)) % delayLine.capacity_frames;
    }

    bool LatencyCompensationManagerImpl::latencyDrainActive() const {
        return latency_drain_active_.load(std::memory_order_acquire);
    }

    int64_t LatencyCompensationManagerImpl::playbackPosition() const {
        return playback_position_samples_.load(std::memory_order_acquire);
    }

    int64_t LatencyCompensationManagerImpl::renderPlaybackPosition() const {
        return render_playback_position_samples_.load(std::memory_order_acquire);
    }

} // namespace uapmd
