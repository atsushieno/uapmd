#include "TrackRoutingManager.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "LatencyCompensationManagerImpl.hpp"

namespace uapmd {

    namespace {
        void applyTrackBusesLayout(SequencerTrack* track, const AudioGraphBusesLayout& layout) {
            if (!track)
                return;
            auto* extension = track->graph().getExtension<AudioBusesLayoutExtension>();
            if (!extension)
                return;
            extension->applyBusesLayout(layout);
        }
    }

    TrackRoutingManager::TrackRoutingManager(
        size_t& audioBufferSizeInFrames,
        int32_t& sampleRate,
        uint32_t& defaultOutputChannels,
        std::vector<std::unique_ptr<SequencerTrack>>& tracks,
        std::unique_ptr<SequencerTrack>& masterTrack,
        std::unique_ptr<AudioProcessContext>& masterTrackContext,
        std::unique_ptr<AudioProcessContext>& mixBusContext,
        SequenceProcessContext& sequence,
        TimelineFacade* timeline,
        LatencyCompensationManager& latencyCompensationManager)
        : audio_buffer_size_in_frames_(audioBufferSizeInFrames)
        , sample_rate_(sampleRate)
        , default_output_channels_(defaultOutputChannels)
        , tracks_(tracks)
        , master_track_(masterTrack)
        , master_track_context_(masterTrackContext)
        , mix_bus_context_(mixBusContext)
        , sequence_(sequence)
        , timeline_(timeline)
        , latency_compensation_manager_(latencyCompensationManager) {
        rebuildRoutingCaches();
    }

    void TrackRoutingManager::rebuildRoutingCaches() {
        track_routing_caches_.assign(tracks_.size(), {});
        max_track_render_lead_in_samples_ = 0;
        max_monitored_live_input_render_lead_in_samples_ = 0;
        max_output_alignment_holdback_in_samples_ = 0;
        output_alignment_active_ = false;

        for (size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex) {
            auto* track = tracks_[trackIndex].get();
            auto& cache = track_routing_caches_[trackIndex];
            if (!track)
                continue;

            const auto outputBusCount = track->graph().outputBusCount();
            cache.effective_targets.resize(outputBusCount);
            cache.path_latencies_in_samples.resize(outputBusCount);
            cache.output_alignment_holdbacks_in_samples.resize(outputBusCount);

            for (uint32_t busIndex = 0; busIndex < outputBusCount; ++busIndex) {
                const auto target = resolveEffectiveTrackOutputBusRoutingTargetUncached(
                    static_cast<uapmd_track_index_t>(trackIndex),
                    busIndex);
                cache.effective_targets[busIndex] = target;
                if (target.type == TrackOutputRoutingTargetType::DISABLED)
                    continue;

                const auto pathLatency =
                    track->graph().outputLatencyInSamples(busIndex) +
                    downstreamLatencyInSamplesForTarget(target);
                cache.path_latencies_in_samples[busIndex] = pathLatency;
                cache.audible_render_lead_in_samples = std::max(
                    cache.audible_render_lead_in_samples,
                    pathLatency);
            }

            max_track_render_lead_in_samples_ = std::max(
                max_track_render_lead_in_samples_,
                cache.audible_render_lead_in_samples);
        }

        if (timeline_)
            for (size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex)
                if (trackUsesLowLatencyMonitoring(static_cast<int32_t>(trackIndex)))
                    max_monitored_live_input_render_lead_in_samples_ = std::max(
                        max_monitored_live_input_render_lead_in_samples_,
                        track_routing_caches_[trackIndex].audible_render_lead_in_samples);

        const bool compensateAgainstMonitoredLiveInput =
            latency_compensation_manager_.playbackCompensationMode() ==
            PlaybackCompensationMode::COMPENSATED;
        for (size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex) {
            auto* track = tracks_[trackIndex].get();
            auto& cache = track_routing_caches_[trackIndex];
            if (!track)
                continue;

            const bool lowLatencyMonitored = trackUsesLowLatencyMonitoring(static_cast<int32_t>(trackIndex));
            for (uint32_t busIndex = 0; busIndex < cache.output_alignment_holdbacks_in_samples.size(); ++busIndex) {
                const auto target = cache.effective_targets[busIndex];
                if (target.type == TrackOutputRoutingTargetType::DISABLED)
                    continue;

                const uint32_t pathLatency = cache.path_latencies_in_samples[busIndex];
                const uint32_t intraTrackHoldback =
                    cache.audible_render_lead_in_samples > pathLatency
                    ? cache.audible_render_lead_in_samples - pathLatency
                    : 0;
                uint32_t holdback = intraTrackHoldback;
                if (compensateAgainstMonitoredLiveInput &&
                    max_monitored_live_input_render_lead_in_samples_ > 0 &&
                    !lowLatencyMonitored)
                    holdback += max_monitored_live_input_render_lead_in_samples_;
                cache.output_alignment_holdbacks_in_samples[busIndex] = holdback;
                max_output_alignment_holdback_in_samples_ = std::max(
                    max_output_alignment_holdback_in_samples_,
                    holdback);
                output_alignment_active_ = output_alignment_active_ || holdback > 0;
            }
        }
    }

    bool TrackRoutingManager::trackUsesLowLatencyMonitoring(uapmd_track_index_t trackIndex) const {
        if (!timeline_ || trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return false;
        if (!timeline_->trackHasLiveInput(trackIndex))
            return false;
        if (latency_compensation_manager_.inputMonitoringPolicy() == InputMonitoringPolicy::OFF)
            return false;
        if (latency_compensation_manager_.playbackCompensationMode() == PlaybackCompensationMode::LOW_LATENCY)
            return true;
        if (latency_compensation_manager_.inputMonitoringPolicy() == InputMonitoringPolicy::TAPE_STYLE)
            return latency_compensation_manager_.trackRecordArmed(trackIndex) &&
                   latency_compensation_manager_.trackMonitoringEnabled(trackIndex);
        return latency_compensation_manager_.trackMonitoringEnabled(trackIndex);
    }

    OutputRoutingExtension* TrackRoutingManager::outputRoutingExtensionForTrackIndex(uapmd_track_index_t trackIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return nullptr;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        return track ? track->graph().getExtension<OutputRoutingExtension>() : nullptr;
    }

    std::vector<TrackOutputRoutingRule> TrackRoutingManager::trackOutputRoutingRules(uapmd_track_index_t trackIndex) const {
        auto* extension = outputRoutingExtensionForTrackIndex(trackIndex);
        return extension ? extension->outputRoutingRules() : std::vector<TrackOutputRoutingRule>{};
    }

    void TrackRoutingManager::setTrackOutputRoutingRules(
        uapmd_track_index_t trackIndex,
        const std::vector<TrackOutputRoutingRule>& rules) {
        if (auto* extension = outputRoutingExtensionForTrackIndex(trackIndex))
            extension->outputRoutingRules(rules);
    }

    TrackOutputRoutingTarget TrackRoutingManager::authoredTrackOutputBusRoutingTarget(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        auto* extension = outputRoutingExtensionForTrackIndex(trackIndex);
        if (!extension)
            return {};
        return extension->outputRoutingTarget(outputBusIndex);
    }

    TrackOutputRoutingTarget TrackRoutingManager::effectiveTrackOutputBusRoutingTarget(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex >= 0 && static_cast<size_t>(trackIndex) < track_routing_caches_.size()) {
            const auto& cache = track_routing_caches_[static_cast<size_t>(trackIndex)];
            if (outputBusIndex < cache.effective_targets.size())
                return cache.effective_targets[outputBusIndex];
        }

        return resolveEffectiveTrackOutputBusRoutingTargetUncached(trackIndex, outputBusIndex);
    }

    TrackOutputRoutingTarget TrackRoutingManager::resolveEffectiveTrackOutputBusRoutingTargetUncached(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {

        const auto authored = authoredTrackOutputBusRoutingTarget(trackIndex, outputBusIndex);
        if (authored.type != TrackOutputRoutingTargetType::DISABLED) {
            if (authored.type == TrackOutputRoutingTargetType::MASTER_INPUT_BUS) {
                if (master_track_ && master_track_context_ && !master_track_->orderedInstanceIds().empty() &&
                    master_track_context_->audioInBusCount() > 0) {
                    const auto busIndex = std::min<uint32_t>(
                        authored.bus_index,
                        static_cast<uint32_t>(master_track_context_->audioInBusCount() - 1));
                    return {
                        TrackOutputRoutingTargetType::MASTER_INPUT_BUS,
                        busIndex,
                        authored.bus_index >= static_cast<uint32_t>(master_track_context_->audioInBusCount()),
                    };
                }
                return {
                    TrackOutputRoutingTargetType::MAIN_MIX_BUS,
                    0,
                    true,
                };
            }

            return {
                TrackOutputRoutingTargetType::MAIN_MIX_BUS,
                0,
                authored.bus_index != 0,
            };
        }

        return trackOutputBusRoutingTargetImpl(trackIndex, outputBusIndex);
    }

    uint32_t TrackRoutingManager::downstreamLatencyInSamplesForTarget(const TrackOutputRoutingTarget& target) const {
        return target.type == TrackOutputRoutingTargetType::MASTER_INPUT_BUS
            ? (master_track_ ? master_track_->renderLeadInSamples() : 0)
            : 0;
    }

    double TrackRoutingManager::downstreamTailLengthInSecondsForTarget(const TrackOutputRoutingTarget& target) const {
        return target.type == TrackOutputRoutingTargetType::MASTER_INPUT_BUS
            ? (master_track_ ? master_track_->tailLengthInSeconds() : 0.0)
            : 0.0;
    }

    uint32_t TrackRoutingManager::fallbackTrackOutputBusPathLatencyInSamples(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return 0;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        if (!track || outputBusIndex >= track->graph().outputBusCount())
            return 0;
        const auto target = effectiveTrackOutputBusRoutingTarget(trackIndex, outputBusIndex);
        if (target.type == TrackOutputRoutingTargetType::DISABLED)
            return 0;
        return track->graph().outputLatencyInSamples(outputBusIndex) + downstreamLatencyInSamplesForTarget(target);
    }

    uint32_t TrackRoutingManager::fallbackTrackAudibleRenderLeadInSamples(uapmd_track_index_t trackIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return 0;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        if (!track)
            return 0;
        uint32_t maxLead = 0;
        for (uint32_t busIndex = 0; busIndex < track->graph().outputBusCount(); ++busIndex) {
            const auto target = effectiveTrackOutputBusRoutingTarget(trackIndex, busIndex);
            if (target.type == TrackOutputRoutingTargetType::DISABLED)
                continue;
            maxLead = std::max(maxLead, fallbackTrackOutputBusPathLatencyInSamples(trackIndex, busIndex));
        }
        return maxLead;
    }

    uint32_t TrackRoutingManager::maxTrackRenderLeadInSamples() const {
        return max_track_render_lead_in_samples_;
    }

    uint32_t TrackRoutingManager::maxLiveInputRenderLeadInSamples() const {
        return max_monitored_live_input_render_lead_in_samples_;
    }

    uint32_t TrackRoutingManager::maxOutputAlignmentHoldbackInSamples() const {
        return max_output_alignment_holdback_in_samples_;
    }

    uint32_t TrackRoutingManager::fallbackTrackOutputAlignmentHoldbackInSamples(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size() || !timeline_)
            return 0;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        if (!track || outputBusIndex >= track->graph().outputBusCount())
            return 0;

        const uint32_t trackLead = fallbackTrackAudibleRenderLeadInSamples(trackIndex);
        const uint32_t pathLatency = fallbackTrackOutputBusPathLatencyInSamples(trackIndex, outputBusIndex);
        const uint32_t intraTrackHoldback = trackLead > pathLatency ? trackLead - pathLatency : 0;
        if (latency_compensation_manager_.playbackCompensationMode() !=
            PlaybackCompensationMode::COMPENSATED)
            return intraTrackHoldback;

        const uint32_t liveInputReferenceLead = maxLiveInputRenderLeadInSamples();
        if (liveInputReferenceLead == 0)
            return intraTrackHoldback;
        if (trackUsesLowLatencyMonitoring(trackIndex))
            return 0;
        return liveInputReferenceLead + intraTrackHoldback;
    }

    bool TrackRoutingManager::isOutputAlignmentActive() const {
        return output_alignment_active_;
    }

    uint32_t TrackRoutingManager::cachedTrackOutputBusPathLatencyInSamples(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= track_routing_caches_.size())
            return 0;
        const auto& cache = track_routing_caches_[static_cast<size_t>(trackIndex)];
        if (outputBusIndex >= cache.path_latencies_in_samples.size())
            return 0;
        return cache.path_latencies_in_samples[outputBusIndex];
    }

    uint32_t TrackRoutingManager::trackAudibleRenderLeadInSamples(uapmd_track_index_t trackIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= track_routing_caches_.size())
            return 0;
        return track_routing_caches_[static_cast<size_t>(trackIndex)].audible_render_lead_in_samples;
    }

    uint32_t TrackRoutingManager::trackOutputAlignmentHoldbackInSamples(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= track_routing_caches_.size())
            return 0;
        const auto& cache = track_routing_caches_[static_cast<size_t>(trackIndex)];
        if (outputBusIndex >= cache.output_alignment_holdbacks_in_samples.size())
            return 0;
        return cache.output_alignment_holdbacks_in_samples[outputBusIndex];
    }

    TrackOutputRoutingTarget TrackRoutingManager::trackOutputBusRoutingTargetImpl(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return {};
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        if (!track || outputBusIndex >= track->graph().outputBusCount())
            return {};

        if (master_track_ && master_track_context_ && master_track_context_->audioInBusCount() > 0) {
            if (outputBusIndex < static_cast<uint32_t>(master_track_context_->audioInBusCount()))
                return {
                    TrackOutputRoutingTargetType::MASTER_INPUT_BUS,
                    outputBusIndex,
                    false,
                };
            return {
                TrackOutputRoutingTargetType::MASTER_INPUT_BUS,
                0,
                true,
            };
        }

        return {
            TrackOutputRoutingTargetType::MAIN_MIX_BUS,
            0,
            outputBusIndex != 0,
        };
    }

    double TrackRoutingManager::tailLengthSecondsToSamples(double seconds) const {
        if (!(seconds > 0.0))
            return 0.0;
        if (!std::isfinite(seconds))
            return std::numeric_limits<double>::infinity();
        return std::ceil(seconds * static_cast<double>(sample_rate_));
    }

    int64_t TrackRoutingManager::maxStopDrainInSamples() const {
        double totalDrainSamples =
            static_cast<double>(master_track_ ? master_track_->renderLeadInSamples() : 0) +
            tailLengthSecondsToSamples(master_track_ ? master_track_->tailLengthInSeconds() : 0.0);

        for (size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex) {
            auto* track = tracks_[trackIndex].get();
            if (!track)
                continue;

            for (uint32_t busIndex = 0; busIndex < track->graph().outputBusCount(); ++busIndex) {
                const auto target = effectiveTrackOutputBusRoutingTarget(static_cast<uapmd_track_index_t>(trackIndex), busIndex);
                if (target.type == TrackOutputRoutingTargetType::DISABLED)
                    continue;

                const double trackPathSamples =
                    static_cast<double>(cachedTrackOutputBusPathLatencyInSamples(static_cast<uapmd_track_index_t>(trackIndex), busIndex)) +
                    tailLengthSecondsToSamples(track->tailLengthInSeconds()) +
                    tailLengthSecondsToSamples(downstreamTailLengthInSecondsForTarget(target));
                if (!std::isfinite(trackPathSamples))
                    return latency_compensation_manager_.realtimeInfiniteTailPolicy() ==
                        RealtimeInfiniteTailPolicy::IMMEDIATE_STOP
                        ? 0
                        : static_cast<int64_t>(std::max(
                            maxTrackRenderLeadInSamples(),
                            master_track_ ? master_track_->renderLeadInSamples() : 0));
                totalDrainSamples = std::max(totalDrainSamples, trackPathSamples);
            }
        }

        if (!std::isfinite(totalDrainSamples))
            return latency_compensation_manager_.realtimeInfiniteTailPolicy() ==
                RealtimeInfiniteTailPolicy::IMMEDIATE_STOP
                ? 0
                : static_cast<int64_t>(std::max(
                    maxTrackRenderLeadInSamples(),
                    master_track_ ? master_track_->renderLeadInSamples() : 0));
        if (totalDrainSamples <= 0.0)
            return 0;
        if (totalDrainSamples >= static_cast<double>(std::numeric_limits<int64_t>::max()))
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(totalDrainSamples);
    }

    void TrackRoutingManager::reconfigureMasterTrackInputBuses() {
        if (!master_track_context_ || !mix_bus_context_)
            return;
        rebuildRoutingCaches();

        std::vector<remidy::AudioBusSpec> mergedInputSpecs(
            master_track_context_->audioInputSpecs().begin(),
            master_track_context_->audioInputSpecs().end());
        for (size_t trackIndex = 0; trackIndex < sequence_.tracks.size() && trackIndex < tracks_.size(); ++trackIndex) {
            auto* ctx = sequence_.tracks[trackIndex];
            auto* track = tracks_[trackIndex].get();
            if (!ctx || !track)
                continue;
            const auto& outputSpecs = ctx->audioOutputSpecs();
            for (uint32_t busIndex = 0; busIndex < track->graph().outputBusCount() && busIndex < outputSpecs.size(); ++busIndex) {
                const auto target = effectiveTrackOutputBusRoutingTarget(static_cast<uapmd_track_index_t>(trackIndex), busIndex);
                if (target.type != TrackOutputRoutingTargetType::MASTER_INPUT_BUS)
                    continue;
                const auto& outputSpec = outputSpecs[busIndex];
                const auto targetBusIndex = static_cast<size_t>(target.bus_index);
                if (targetBusIndex >= mergedInputSpecs.size()) {
                    mergedInputSpecs.resize(targetBusIndex + 1, outputSpec);
                    mergedInputSpecs[targetBusIndex] = outputSpec;
                    continue;
                }
                mergedInputSpecs[targetBusIndex].channels = std::max(mergedInputSpecs[targetBusIndex].channels, outputSpec.channels);
                mergedInputSpecs[targetBusIndex].bufferCapacityFrames = std::max(
                    mergedInputSpecs[targetBusIndex].bufferCapacityFrames,
                    outputSpec.bufferCapacityFrames);
                if (outputSpec.role == remidy::AudioBusRole::Main)
                    mergedInputSpecs[targetBusIndex].role = remidy::AudioBusRole::Main;
            }
        }

        if (mergedInputSpecs != master_track_context_->audioInputSpecs())
            master_track_context_->configureAudioInputBuses(mergedInputSpecs);
        if (master_track_)
            applyTrackBusesLayout(master_track_.get(), AudioGraphBusesLayout{
                static_cast<uint32_t>(master_track_context_->audioInBusCount()),
                static_cast<uint32_t>(master_track_context_->audioOutBusCount()),
                1,
                1,
            });
    }

    void TrackRoutingManager::reconfigureMixBusContext() {
        if (!mix_bus_context_)
            return;
        rebuildRoutingCaches();

        std::vector<remidy::AudioBusSpec> mixSpecs;
        for (size_t trackIndex = 0; trackIndex < sequence_.tracks.size() && trackIndex < tracks_.size(); ++trackIndex) {
            const auto* ctx = sequence_.tracks[trackIndex];
            auto* track = tracks_[trackIndex].get();
            if (!ctx || !track)
                continue;
            const auto& outputSpecs = ctx->audioOutputSpecs();
            for (uint32_t busIndex = 0; busIndex < track->graph().outputBusCount() && busIndex < outputSpecs.size(); ++busIndex) {
                const auto target = effectiveTrackOutputBusRoutingTarget(static_cast<uapmd_track_index_t>(trackIndex), busIndex);
                if (target.type != TrackOutputRoutingTargetType::MAIN_MIX_BUS)
                    continue;
                const auto& spec = outputSpecs[busIndex];
                const auto targetBusIndex = static_cast<size_t>(target.bus_index);
                if (targetBusIndex >= mixSpecs.size()) {
                    mixSpecs.resize(targetBusIndex + 1, spec);
                    mixSpecs[targetBusIndex] = spec;
                    continue;
                }
                mixSpecs[targetBusIndex].channels = std::max(mixSpecs[targetBusIndex].channels, spec.channels);
                mixSpecs[targetBusIndex].bufferCapacityFrames = std::max(
                    mixSpecs[targetBusIndex].bufferCapacityFrames,
                    spec.bufferCapacityFrames);
                if (spec.role == remidy::AudioBusRole::Main)
                    mixSpecs[targetBusIndex].role = remidy::AudioBusRole::Main;
            }
        }

        if (mixSpecs.empty())
            mixSpecs.push_back(
                remidy::AudioBusSpec{
                    remidy::AudioBusRole::Main,
                    default_output_channels_,
                    audio_buffer_size_in_frames_});
        mix_bus_context_->configureAudioOutputBuses(mixSpecs);
        reconfigureMasterTrackInputBuses();
    }

} // namespace uapmd
