#include <algorithm>
#include <cstring>
#include <type_traits>
#include <aap/ext/midi.h>

#include "PluginFormatAAP.hpp"

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

void remidy::PluginInstanceAAP::installTailSupport() {
    static_assert(std::is_standard_layout_v<TailHostExtension>);
    tail_host_extension_ = {{nullptr, staticNotifyTailLengthChanged}, this};
    auto* remote = dynamic_cast<aap::RemotePluginInstance*>(instance);
    if (!remote || !remote->getPluginInformation()->hasExtension(AAP_TAIL_EXTENSION_URI))
        return;
    remote->getHostExtension = [this](aap::RemotePluginInstance*, uint8_t, const char* uri) -> void* {
        return uri && !strcmp(uri, AAP_TAIL_EXTENSION_URI) ? &tail_host_extension_.ext : nullptr;
    };
}

void remidy::PluginInstanceAAP::uninstallTailSupport() {
    tail_state_->owner.store(nullptr, std::memory_order_release);
    if (auto* remote = dynamic_cast<aap::RemotePluginInstance*>(instance))
        remote->getHostExtension = nullptr;
}

void remidy::PluginInstanceAAP::refreshTailLength() {
    auto* remote = dynamic_cast<aap::RemotePluginInstance*>(instance);
    if (!remote || !remote->getPluginInformation()->hasExtension(AAP_TAIL_EXTENSION_URI))
        return;
    auto* aapxs = remote->getAAPXSDispatcher().getPluginAAPXSByUri(AAP_TAIL_EXTENSION_URI);
    auto* tail = aapxs ? dynamic_cast<aap::xs::TailClientAAPXS*>(static_cast<aap::xs::TypedAAPXS*>(aapxs->typed_client)) : nullptr;
    if (!tail)
        return;
    // Async on purpose: while ACTIVE the request travels on the audio cycle, so blocking here could stall.
    tail->getTailLengthAsync([weak = std::weak_ptr<TailState>(tail_state_)](aap::Result<double> result) {
        auto state = weak.lock();
        if (!state || !result.isOk())
            return;
        if (state->seconds.exchange(result.value, std::memory_order_acq_rel) == result.value)
            return;
        EventLoop::enqueueTaskOnMainThread([weak] {
            auto state = weak.lock();
            if (auto* owner = state ? state->owner.load(std::memory_order_acquire) : nullptr)
                owner->notifyTimingInfoChanged({.tail_changed = true});
        });
    });
}

void remidy::PluginInstanceAAP::staticNotifyTailLengthChanged(aap_tail_host_extension_t* ext, AndroidAudioPluginHost*) {
    // It arrives on an AAPXS worker thread; query the new length from the main thread.
    auto* owner = reinterpret_cast<TailHostExtension*>(ext)->owner;
    EventLoop::enqueueTaskOnMainThread([weak = std::weak_ptr<TailState>(owner->tail_state_)] {
        auto state = weak.lock();
        if (auto* owner = state ? state->owner.load(std::memory_order_acquire) : nullptr)
            owner->refreshTailLength();
    });
}

remidy::PluginInstanceAAP::PluginInstanceAAP(
        PluginFormatAAPImpl* format, PluginCatalogEntry* entry, aap::PluginInstance* aapInstance
) : PluginInstance(entry), format(format), instance(aapInstance) {
    extensibility_ = std::make_unique<Extensibility>(*this);
    extensibility_->refresh();
    installTailSupport();
}

remidy::PluginInstanceAAP::~PluginInstanceAAP() {
    uninstallTailSupport();
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

    if (instance->getInstanceState() == aap::PLUGIN_INSTANTIATION_STATE_ACTIVE)
        instance->deactivate();
    if (instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_UNPREPARED &&
        instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_INACTIVE)
        return StatusCode::FAILED_TO_CONFIGURE;

    if (auto* remote = dynamic_cast<aap::RemotePluginInstance*>(instance);
        remote && remote->isBusMode()) {
        // Pick up plugin layout changes only here, while processing is stopped.
        auto error = remote->refreshBusLayout();
        if (!error.empty() && instance->getInstanceState() == aap::PLUGIN_INSTANTIATION_STATE_ERROR)
            return StatusCode::FAILED_TO_CONFIGURE;
    }

    auto* audioBuses = static_cast<PluginBusesAAP*>(this->audioBuses());
    audioBuses->configure(configuration);
    if (instance->getInstanceState() == aap::PLUGIN_INSTANTIATION_STATE_UNPREPARED)
        instance->prepare((int) configuration.bufferSizeInSamples, (int32_t) configuration.sampleRate);

    if (instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_INACTIVE ||
        instance->getAudioPluginBuffer() == nullptr)
        return StatusCode::FAILED_TO_INSTANTIATE;

    use_double = configuration.dataType == AudioContentType::Float64;

    refreshTailLength();

    return StatusCode::OK;
}

void remidy::PluginInstanceAAP::setAudioProcessingMode(AudioProcessingMode mode) {
    auto* instance = aapInstance();
    if (!instance)
        return; // FIXME: should we quietly finish?
    if (instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_INACTIVE)
        return; // FIXME: should we quietly finish?
    auto aapMode = mode == AudioProcessingMode::Realtime ? AAP_OFFLINE_RENDERING_MODE_REALTIME : AAP_OFFLINE_RENDERING_MODE_OFFLINE;
    instance->getStandardExtensions().setOfflineMode(aapMode);
}

remidy::StatusCode remidy::PluginInstanceAAP::startProcessing() {
    auto* instance = aapInstance();
    if (!instance)
        return StatusCode::FAILED_TO_START_PROCESSING;
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
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::process(remidy::AudioProcessContext &process) {
    auto instance = aapInstance();
    if (!instance)
        return StatusCode::FAILED_TO_PROCESS;
    auto buffer = instance->getAudioPluginBuffer();
    if (!buffer || !buses || process.frameCount() < 0 || process.frameCount() > buffer->num_frames(buffer))
        return StatusCode::FAILED_TO_PROCESS;

    const auto frames = process.frameCount();
    const auto& inputBuses = buses->audioInputBuses();
    for (int32_t bus = 0, n = buffer->get_bus_count(buffer, AAP_BUS_KIND_AUDIO, AAP_PORT_DIRECTION_INPUT); bus < n; bus++) {
        auto channels = buffer->get_audio_channels(buffer, AAP_PORT_DIRECTION_INPUT, bus);
        const bool enabled = static_cast<size_t>(bus) < inputBuses.size() && inputBuses[bus]->enabled();
        const auto hostChannels = enabled ? process.inputChannelCount(bus) : 0;
        if (hostChannels > 0 && process.inputBusBufferCapacityInFrames(bus) < static_cast<size_t>(frames))
            return StatusCode::FAILED_TO_PROCESS;
        for (int32_t channel = 0, count = buffer->get_audio_channel_count(buffer, AAP_PORT_DIRECTION_INPUT, bus); channel < count; channel++) {
            auto* dst = channels ? channels[channel] : nullptr;
            if (!dst)
                continue;
            if (channel >= hostChannels)
                std::fill_n(dst, frames, 0.0f);
            else if (use_double)
                std::copy_n(process.getDoubleInBuffer(bus, channel), frames, dst);
            else
                std::copy_n(process.getFloatInBuffer(bus, channel), frames, dst);
        }
    }
    if (buffer->get_bus_count(buffer, AAP_BUS_KIND_EVENT, AAP_PORT_DIRECTION_INPUT) > 0) {
        auto& eIn = process.eventIn();
        instance->addEventUmpInput(eIn.getMessages(), eIn.position());
    }

    instance->process(process.frameCount(), 0);

    const auto& outputBuses = buses->audioOutputBuses();
    for (int32_t bus = 0, n = process.audioOutBusCount(); bus < n; bus++) {
        if (process.outputChannelCount(bus) > 0 &&
            process.outputBusBufferCapacityInFrames(bus) < static_cast<size_t>(frames))
            return StatusCode::FAILED_TO_PROCESS;
        const bool enabled = static_cast<size_t>(bus) < outputBuses.size() && outputBuses[bus]->enabled();
        auto channels = buffer->get_audio_channels(buffer, AAP_PORT_DIRECTION_OUTPUT, bus);
        const auto count = enabled ? buffer->get_audio_channel_count(buffer, AAP_PORT_DIRECTION_OUTPUT, bus) : 0;
        for (int32_t channel = 0, hostChannels = process.outputChannelCount(bus); channel < hostChannels; channel++) {
            auto* src = channels && channel < count ? channels[channel] : nullptr;
            if (use_double) {
                auto* dst = process.getDoubleOutBuffer(bus, channel);
                if (src)
                    std::copy_n(src, frames, dst);
                else
                    std::fill_n(dst, frames, 0.0);
            } else {
                auto* dst = process.getFloatOutBuffer(bus, channel);
                if (src)
                    std::copy_n(src, frames, dst);
                else
                    std::fill_n(dst, frames, 0.0f);
            }
        }
    }
    if (buffer->get_bus_count(buffer, AAP_BUS_KIND_EVENT, AAP_PORT_DIRECTION_OUTPUT) > 0) {
        auto& eOut = process.eventOut();
        auto src = buffer->get_event_buffer(buffer, AAP_PORT_DIRECTION_OUTPUT, 0);
        auto size = buffer->get_event_buffer_capacity(buffer, AAP_PORT_DIRECTION_OUTPUT, 0);
        if (src && size > static_cast<int32_t>(sizeof(AAPMidiBufferHeader))) {
            auto* header = reinterpret_cast<AAPMidiBufferHeader*>(src);
            auto* payload = reinterpret_cast<uint8_t*>(header + 1);
            const auto availableBytes = std::min<size_t>(header->length, size - sizeof(AAPMidiBufferHeader));
            const auto payloadBytes = std::min<size_t>(availableBytes, eOut.maxMessagesInBytes());
            if (payloadBytes > 0) {
                std::memcpy(eOut.getMessages(), payload, payloadBytes);
                eOut.position(payloadBytes);
                if (params)
                    params->ingestPluginParameterUpdates(payload, availableBytes);
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
