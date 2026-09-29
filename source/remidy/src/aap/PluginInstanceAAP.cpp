#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <aap/ext/midi.h>

#include "PluginFormatAAP.hpp"

namespace {
    int64_t monotonicNanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr uint32_t kPerformanceHintQueryInterval = 8;
}

void remidy::PluginInstanceAAP::Extensibility::refresh() {
    plugin_package_name_.clear();
    plugin_local_name_.clear();
    auto* aap = owner_.aapInstance();
    if (!aap)
        return;
    auto* info = aap->getPluginInformation();
    if (!info)
        return;
    if (std::string package_name = info->getPluginPackageName(); !package_name.empty())
        plugin_package_name_ = package_name;
    if (std::string local_name = info->getPluginLocalName(); !local_name.empty())
        plugin_local_name_ = local_name;
}

int32_t remidy::PluginInstanceAAP::Extensibility::instanceId() const {
    auto* aap = owner_.aapInstance();
    return aap ? aap->getInstanceId() : -1;
}

remidy::PluginInstanceAAP::PluginInstanceAAP(
        PluginFormatAAPImpl* format, PluginCatalogEntry* entry, aap::PluginInstance* aapInstance
) : PluginInstance(entry), format(format), instance(aapInstance) {
    extensibility_ = std::make_unique<Extensibility>(*this);
    extensibility_->refresh();
}

remidy::PluginInstanceAAP::~PluginInstanceAAP() {
    if (!format || !instance)
        return;
    format->destroyInstance(instance);
    instance = nullptr;
}

remidy::StatusCode
remidy::PluginInstanceAAP::configure(remidy::PluginInstance::ConfigurationRequest &configuration) {
    auto* instance = aapInstance();
    if (!instance)
        return StatusCode::FAILED_TO_INSTANTIATE;

    configured_frames_ = configuration.bufferSizeInSamples;
    configured_sample_rate_ = configuration.sampleRate;
    configured_offline_ = configuration.offlineMode;

    if (instance->getInstanceState() == aap::PLUGIN_INSTANTIATION_STATE_UNPREPARED)
        instance->prepare((int) configuration.bufferSizeInSamples, (int32_t) configuration.sampleRate);
    else if (instance->getInstanceState() == aap::PLUGIN_INSTANTIATION_STATE_ACTIVE)
        instance->deactivate();

    if (instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_INACTIVE ||
        instance->getAudioPluginBuffer() == nullptr)
        return StatusCode::FAILED_TO_INSTANTIATE;

    // generate port mappings
    remidy_to_aap_port_index_map_audio_in.clear();
    remidy_to_aap_port_index_map_audio_out.clear();

    for (auto i = 0, n = instance->getNumPorts(); i < n; i++) {
        auto port = instance->getPort(i);
        if (port->getContentType() == AAP_CONTENT_TYPE_AUDIO) {
            if (port->getPortDirection() == AAP_PORT_DIRECTION_INPUT)
                remidy_to_aap_port_index_map_audio_in.push_back(i);
            if (port->getPortDirection() == AAP_PORT_DIRECTION_OUTPUT)
                remidy_to_aap_port_index_map_audio_out.push_back(i);
        }
        if (port->getContentType() == AAP_CONTENT_TYPE_MIDI2) {
            if (port->getPortDirection() == AAP_PORT_DIRECTION_INPUT)
                aap_port_midi2_in = i;
            if (port->getPortDirection() == AAP_PORT_DIRECTION_OUTPUT)
                aap_port_midi2_out = i;
        }
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::startProcessing() {
    auto* instance = aapInstance();
    if (!instance)
        return StatusCode::FAILED_TO_START_PROCESSING;
    // the performance-hint configure request has to be sent before the instance becomes ACTIVE.
    configurePerformanceHint(PerformanceHintCoordinator::enabled() && !configured_offline_);
    instance->activate();
    if (instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_ACTIVE)
        return StatusCode::FAILED_TO_START_PROCESSING;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::stopProcessing() {
    auto* instance = aapInstance();
    if (!instance)
        return StatusCode::FAILED_TO_STOP_PROCESSING;
    instance->deactivate();
    if (performance_hint_status_ == AAP_PERFORMANCE_HINT_STATUS_ACTIVE)
        configurePerformanceHint(false);
    return StatusCode::OK;
}

int64_t remidy::PluginInstanceAAP::blockDurationNanos() const {
    if (configured_sample_rate_ == 0)
        return 0;
    return static_cast<int64_t>(configured_frames_) * 1000000000 / configured_sample_rate_;
}

bool remidy::PluginInstanceAAP::serviceSupportsPerformanceHint() const {
    // A service built with an older libandroidaudioplugin crashes on unknown extension requests, so it must be declared.
    auto* info = instance->getPluginInformation();
    if (!info)
        return false;
    for (int i = 0, n = info->getNumExtensions(); i < n; i++)
        if (info->getExtension(i).uri == AAP_PERFORMANCE_HINT_EXTENSION_URI)
            return true;
    return false;
}

void remidy::PluginInstanceAAP::configurePerformanceHint(bool enabled) {
    auto* client = instance->getStandardExtensions().getPerformanceHintClient();
    const auto block = blockDurationNanos();
    if (!client || block <= 0 || !serviceSupportsPerformanceHint()) {
        performance_hint_status_ = AAP_PERFORMANCE_HINT_STATUS_UNSUPPORTED;
        return;
    }
    performance_hint_status_ = client->configure(enabled, block);
    performance_hint_sent_target_nanos_ = block;
    performance_hint_workload_serial_ = PerformanceHintCoordinator::workloadSerial();
    performance_hint_query_countdown_ = 0;
}

int64_t remidy::PluginInstanceAAP::sendPerformanceHintRequests() {
    // RT-safe requests are delivered as AAPXS SysEx8 in the MIDI2 input, and a service that did not confirm support must not receive them.
    if (performance_hint_status_ != AAP_PERFORMANCE_HINT_STATUS_ACTIVE || aap_port_midi2_in < 0)
        return 0;
    auto* client = instance->getStandardExtensions().getPerformanceHintClient();
    if (!client)
        return 0;

    if (const auto serial = PerformanceHintCoordinator::workloadSerial(); serial != performance_hint_workload_serial_) {
        performance_hint_workload_serial_ = serial;
        switch (PerformanceHintCoordinator::workload()) {
            case PerformanceWorkloadHint::Increase:
                client->notifyWorkload(AAP_PERFORMANCE_HINT_WORKLOAD_INCREASE);
                break;
            case PerformanceWorkloadHint::Spike:
                client->notifyWorkload(AAP_PERFORMANCE_HINT_WORKLOAD_SPIKE);
                break;
            case PerformanceWorkloadHint::Reset:
                client->notifyWorkload(AAP_PERFORMANCE_HINT_WORKLOAD_RESET);
                break;
            default:
                break;
        }
    }

    const auto timing = client->getTiming();
    const auto remoteNanos = std::max(timing.last_duration_nanos, timing.max_duration_nanos);
    const auto scale = PerformanceHintCoordinator::remoteBudgetScale();
    const auto block = blockDurationNanos();
    if (remoteNanos > 0 && scale > 0 && block > 0) {
        const auto target = std::clamp<int64_t>(static_cast<int64_t>(static_cast<double>(remoteNanos) * scale), block / 10, block);
        // skip changes within 10% so that the target is not updated on every block.
        if (std::llabs(target - performance_hint_sent_target_nanos_) * 10 > performance_hint_sent_target_nanos_) {
            client->setTarget(target);
            performance_hint_sent_target_nanos_ = target;
        }
    }

    if (performance_hint_query_countdown_ == 0) {
        client->queryTiming();
        performance_hint_query_countdown_ = kPerformanceHintQueryInterval;
    }
    --performance_hint_query_countdown_;
    return remoteNanos;
}

remidy::StatusCode remidy::PluginInstanceAAP::process(remidy::AudioProcessContext &process) {
    size_t aapIdx;
    auto instance = aapInstance();
    if (!instance)
        return StatusCode::FAILED_TO_PROCESS;
    auto buffer = instance->getAudioPluginBuffer();
    if (!buffer)
        return StatusCode::FAILED_TO_PROCESS;

    aapIdx = 0;
    for (auto iBus = 0, nBus = process.audioInBusCount(); iBus < nBus; iBus++) {
        for (auto iCh = 0, nCh = process.inputChannelCount(iBus); iCh < nCh; iCh++) {
            if (remidy_to_aap_port_index_map_audio_in.size() <= aapIdx)
                break;
            auto aapPortIdx = remidy_to_aap_port_index_map_audio_in[aapIdx++];
            auto src = process.getFloatInBuffer(iBus, iCh);
            auto dst = buffer->get_buffer(buffer, aapPortIdx);
            memcpy(dst, src, sizeof(float) * process.frameCount());
        }
        // FIXME: we should iterate non-main buses too once AAP is ready for that.
        break;
    }
    if (aap_port_midi2_in >= 0) {
        auto& eIn = process.eventIn();
        instance->addEventUmpInput(eIn.getMessages(), eIn.position());
    }

    const auto remoteProcessingNanos = sendPerformanceHintRequests();
    const auto processBegin = monotonicNanos();
    instance->process(process.frameCount(), 0);
    PerformanceHintCoordinator::addRemoteProcessing(monotonicNanos() - processBegin, remoteProcessingNanos);

    aapIdx = 0;
    for (auto iBus = 0, nBus = process.audioOutBusCount(); iBus < nBus; iBus++) {
        for (auto iCh = 0, nCh = process.outputChannelCount(iBus); iCh < nCh; iCh++) {
            if (remidy_to_aap_port_index_map_audio_out.size() <= aapIdx)
                break;
            auto aapPortIdx = remidy_to_aap_port_index_map_audio_out[aapIdx++];
            auto dst = process.getFloatOutBuffer(iBus, iCh);
            auto src = buffer->get_buffer(buffer, aapPortIdx);
            memcpy(dst, src, sizeof(float) * process.frameCount());
        }
        // FIXME: we should iterate non-main buses too once AAP is ready for that.
        break;
    }
    if (aap_port_midi2_out >= 0) {
        auto& eOut = process.eventOut();
        auto src = buffer->get_buffer(buffer, aap_port_midi2_out);
        auto size = buffer->get_buffer_size(buffer, aap_port_midi2_out);
        if (src && size > static_cast<int32_t>(sizeof(AAPMidiBufferHeader))) {
            auto* header = reinterpret_cast<AAPMidiBufferHeader*>(src);
            auto* payload = reinterpret_cast<uint8_t*>(header + 1);
            const auto payloadBytes = std::min<size_t>(header->length, eOut.maxMessagesInBytes());
            if (payloadBytes > 0) {
                std::memcpy(eOut.getMessages(), payload, payloadBytes);
                eOut.position(payloadBytes);
                if (auto* params = dynamic_cast<PluginInstanceAAP::ParameterSupport*>(parameters()))
                    params->ingestPluginParameterUpdates(payload, header->length);
            } else {
                eOut.position(0);
            }
            header->length = 0;
        } else {
            eOut.position(0);
        }
    } else {
        process.eventOut().position(0);
    }

    return StatusCode::OK;
}
