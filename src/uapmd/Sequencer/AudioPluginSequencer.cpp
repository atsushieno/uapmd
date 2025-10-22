
#include <iostream>
#include <format>
#include <cmidi2.h>
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/sequencer/AudioPluginSequencer.hpp"
#include "uapmd/priv/plugingraph/AudioPluginHostPAL.hpp"

namespace {
    uapmd::AudioPluginHostPAL::AudioPluginNodePAL* findNodePalByInstanceId(
        uapmd::SequenceProcessor& sequencer,
        int32_t instanceId
    ) {
        for (auto& track : sequencer.tracks()) {
            for (auto node : track->graph().plugins()) {
                if (node->instanceId() == instanceId)
                    return node->pal();
            }
        }
        return nullptr;
    }
}


uapmd::AudioPluginSequencer::AudioPluginSequencer(
    size_t audioBufferSizeInFrames,
    size_t umpBufferSizeInBytes,
    int32_t sampleRate
) : buffer_size_in_frames(audioBufferSizeInFrames),
    ump_buffer_size_in_bytes(umpBufferSizeInBytes), sample_rate(sampleRate),
    plugin_host_pal(AudioPluginHostPAL::instance()),
    sequencer(sampleRate, buffer_size_in_frames, umpBufferSizeInBytes, plugin_host_pal
) {
    auto manager = AudioIODeviceManager::instance();
    auto logger = remidy::Logger::global();
    AudioIODeviceManager::Configuration audioConfig{ .logger = logger };
    manager->initialize(audioConfig);

    // FIXME: enable MIDI devices
    dispatcher.configure(umpBufferSizeInBytes, manager->open());

    dispatcher.addCallback([&](uapmd::AudioProcessContext& process) {
        auto& data = sequencer.data();

        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventOut().position(0); // clean up *out* events here.
            ctx->frameCount(process.frameCount());
            for (uint32_t i = 0; i < process.audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = process.inputChannelCount(i); ch < nCh; ch++)
                    memcpy(ctx->getFloatInBuffer(i, ch),
                           (void *) process.getFloatInBuffer(i, ch), process.frameCount() * sizeof(float));
            }
        }
        auto ret = sequencer.processAudio();

        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.
            for (uint32_t i = 0; i < process.audioOutBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->outputChannelCount(i); ch < nCh; ch++)
                    memcpy(process.getFloatOutBuffer(i, ch), (void*) ctx->getFloatOutBuffer(i, ch), process.frameCount() * sizeof(float));
            }
        }
        return ret;
    });
}

uapmd::AudioPluginSequencer::~AudioPluginSequencer() {
    stopAudio();
}

uapmd::PluginCatalog& uapmd::AudioPluginSequencer::catalog() {
    return plugin_host_pal->catalog();
}

void uapmd::AudioPluginSequencer::performPluginScanning(bool rescan) {
    plugin_host_pal->performPluginScanning(rescan);
}

std::vector<uapmd::ParameterMetadata> uapmd::AudioPluginSequencer::getParameterList(int32_t instanceId) {
    for (auto& track : sequencer.tracks())
        for (auto node : track->graph().plugins())
            if (node->instanceId() == instanceId)
                return node->pal()->parameterMetadataList();
    return {};
}

std::vector<uapmd::PresetsMetadata> uapmd::AudioPluginSequencer::getPresetList(int32_t instanceId) {
    for (auto& track : sequencer.tracks())
        for (auto node : track->graph().plugins())
            if (node->instanceId() == instanceId)
                return node->pal()->presetMetadataList();
    return {};
}

void uapmd::AudioPluginSequencer::loadPreset(int32_t instanceId, int32_t presetIndex) {
    for (auto& track : sequencer.tracks())
        for (auto node : track->graph().plugins())
            if (node->instanceId() == instanceId) {
                // Need to access the underlying plugin instance to call presets()->loadPreset()
                // This requires adding a method to the PAL interface
                node->pal()->loadPreset(presetIndex);
                return;
            }
}

std::vector<int32_t> uapmd::AudioPluginSequencer::getInstanceIds() {
    std::vector<int32_t> instances;
    for (auto& track : sequencer.tracks()) {
        for (auto plugin : track->graph().plugins()) {
            instances.push_back(plugin->instanceId());
        }
    }
    return instances;
}

std::string uapmd::AudioPluginSequencer::getPluginName(int32_t instanceId) {
    for (auto& track : sequencer.tracks()) {
        for (auto plugin : track->graph().plugins()) {
            if (plugin->instanceId() == instanceId) {
                // Get plugin ID and look it up in the catalog
                std::string pluginId = plugin->pal()->pluginId();
                std::string format = plugin->pal()->formatName();

                // Search in the catalog for display name
                auto plugins = plugin_host_pal->catalog().getPlugins();
                for (auto* catalogPlugin : plugins) {
                    if (catalogPlugin->pluginId() == pluginId && catalogPlugin->format() == format)
                        return catalogPlugin->displayName();
                }
                return "Plugin " + std::to_string(instanceId);
            }
        }
    }
    return "Unknown Plugin";
}

std::string uapmd::AudioPluginSequencer::getPluginFormat(int32_t instanceId) {
    for (auto& track : sequencer.tracks()) {
        for (auto plugin : track->graph().plugins()) {
            if (plugin->instanceId() == instanceId) {
                return plugin->pal()->formatName();
            }
        }
    }
    return "";
}

bool uapmd::AudioPluginSequencer::hasPluginUI(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->hasUISupport();
}

bool uapmd::AudioPluginSequencer::createPluginUI(int32_t instanceId, bool isFloating) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->createUI(isFloating);
}

bool uapmd::AudioPluginSequencer::attachPluginUI(int32_t instanceId, void* parentHandle) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->attachUI(parentHandle);
}

bool uapmd::AudioPluginSequencer::showPluginUI(int32_t instanceId, bool isFloating, void* parentHandle) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    if (!pal->createUI(isFloating))
        return false;
    if (!isFloating) {
        if (!parentHandle)
            return false;
        if (!pal->attachUI(parentHandle))
            return false;
    }
    return pal->showUI();
}

void uapmd::AudioPluginSequencer::hidePluginUI(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return;
    pal->hideUI();
}

bool uapmd::AudioPluginSequencer::isPluginUIVisible(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->isUIVisible();
}

void uapmd::AudioPluginSequencer::setPluginUIResizeHandler(int32_t instanceId, std::function<bool(uint32_t, uint32_t)> handler) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return;
    pal->setUIResizeHandler(std::move(handler));
}

bool uapmd::AudioPluginSequencer::resizePluginUI(int32_t instanceId, uint32_t width, uint32_t height) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->setUISize(width, height);
}

bool uapmd::AudioPluginSequencer::getPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->getUISize(width, height);
}

bool uapmd::AudioPluginSequencer::canPluginUIResize(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->canUIResize();
}

void uapmd::AudioPluginSequencer::instantiatePlugin(
    std::string& format,
    std::string& pluginId,
    std::function<void(int32_t instanceId, std::string error)> callback
) {
    sequencer.addSimpleTrack(format, pluginId, [&,callback](AudioPluginTrack* track, std::string error) {
        if (!error.empty()) {
            callback(-1, error);
        } else {
            auto trackCtx = sequencer.data().tracks[sequencer.tracks().size() - 1];
            auto numChannels = dispatcher.audio()->channels();
            trackCtx->configureMainBus(numChannels, numChannels, buffer_size_in_frames);

            callback(track->graph().plugins()[0]->instanceId(), error);
        }
    });
}

void addMessage64(cmidi2_ump* dst, int64_t ump) {
    cmidi2_ump_write64(dst, ump);
}

void uapmd::AudioPluginSequencer::sendNoteOn(int32_t trackIndex, int32_t note) {
    if (trackIndex < 0 || trackIndex >= sequencer.tracks().size()) {
        remidy::Logger::global()->logError("trackIndex is out of range: {}", trackIndex);
        return;
    }
    cmidi2_ump umps[2];
    auto ump = cmidi2_ump_midi2_note_on(0, 0, note, 0, 0xF800, 0);
    addMessage64(umps, ump);
    if (!sequencer.tracks()[trackIndex]->scheduleEvents(0, umps, 8))
        remidy::Logger::global()->logError(std::format("Failed to enqueue note on event {}: {}", trackIndex, note).c_str());

    remidy::Logger::global()->logError(std::format("Native note on {}: {}", trackIndex, note).c_str());
}

void uapmd::AudioPluginSequencer::sendNoteOff(int32_t trackIndex, int32_t note) {
    if (trackIndex < 0 || trackIndex >= sequencer.tracks().size()) {
        remidy::Logger::global()->logError("trackIndex is out of range: {}", trackIndex);
        return;
    }
    cmidi2_ump umps[2];
    auto ump = cmidi2_ump_midi2_note_off(0, 0, note, 0, 0xF800, 0);
    addMessage64(umps, ump);
    if (!sequencer.tracks()[trackIndex]->scheduleEvents(0, umps, 8))
        remidy::Logger::global()->logError(std::format("Failed to enqueue note off event {}: {}", trackIndex, note).c_str());

    remidy::Logger::global()->logError(std::format("Native note off {}: {}", trackIndex, note).c_str());
}

void uapmd::AudioPluginSequencer::setParameterValue(int32_t instanceId, int32_t index, double value) {
    cmidi2_ump umps[2];
    uint32_t vi32 = UINT32_MAX * value;
    auto ump = cmidi2_ump_midi2_nrpn(0, 0, (uint8_t) (index / 0x100), (uint8_t) (index % 0x100), vi32);
    addMessage64(umps, ump);
    for (auto& track : sequencer.tracks())
        for (auto& node : track->graph().plugins())
            if (node->instanceId() == instanceId) {
                node->pal()->setParameterValue(index, value);
                remidy::Logger::global()->logError(std::format("Native parameter change {}: {} = {}", instanceId, index, value).c_str());
                break;
            }
}

void uapmd::AudioPluginSequencer::enqueueUmp(int32_t trackIndex, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
    auto track = sequencer.tracks()[trackIndex];
    if (!track->scheduleEvents(timestamp, ump, sizeInBytes))
        remidy::Logger::global()->logError(std::format("Failed to enqueue UMP events: size {}", sizeInBytes).c_str());
}

uapmd_status_t uapmd::AudioPluginSequencer::startAudio() {
    return dispatcher.start();
}

uapmd_status_t uapmd::AudioPluginSequencer::stopAudio() {
    return dispatcher.stop();
}

uapmd_status_t uapmd::AudioPluginSequencer::isAudioPlaying() {
    return dispatcher.isPlaying();
}

int32_t uapmd::AudioPluginSequencer::sampleRate() { return sample_rate; }
bool uapmd::AudioPluginSequencer::sampleRate(int32_t newSampleRate) {
    if (dispatcher.isPlaying())
        return false;
    sample_rate = newSampleRate;
    return true;
}

void uapmd::AudioPluginSequencer::loadState(std::vector<uint8_t>& state) {
    // FIXME: we need some un-structure
    for (auto track : this->sequencer.tracks())
        for (auto plugin : track->graph().plugins())
            plugin->loadState(state);
}

std::vector<uint8_t> uapmd::AudioPluginSequencer::saveState() {
    std::vector<uint8_t> ret{};
    for (auto track : this->sequencer.tracks())
        for (auto plugin : track->graph().plugins()) {
            // FIXME: we need some structure
            auto target = plugin->saveState();
            std::copy(target.begin(), target.end(), std::back_inserter(ret));
        }
    return ret;
}
