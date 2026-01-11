#include <uapmd/priv/sequencer/AudioPluginSequencer.hpp>
#include <uapmd/priv/sequencer/SequencerEngine.hpp>
#include <uapmd/priv/plugin-api/AudioPluginInstanceAPI.hpp>
#include <memory>
#include <unordered_map>

#include "AppModel.hpp"

namespace uapmd {

// Minimal stub plugin instance implementing API with no-ops
class StubPluginInstance : public AudioPluginInstanceAPI {
    mutable std::string fmt_{"Stub"};
    mutable std::string id_{"stub.plugin"};
public:
    std::string& formatName() const override { return fmt_; }
    std::string& pluginId() const override { return id_; }

    uapmd_status_t processAudio(AudioProcessContext&) override { return 0; }

    std::vector<ParameterMetadata> parameterMetadataList() override { return {}; }
    std::vector<ParameterMetadata> perNoteControllerMetadataList(remidy::PerNoteControllerContextTypes, uint32_t) override { return {}; }
    std::vector<PresetsMetadata> presetMetadataList() override { return {}; }
    void loadPreset(int32_t) override {}

    std::vector<uint8_t> saveState() override { return {}; }
    void loadState(std::vector<uint8_t>&) override {}

    double getParameterValue(int32_t) override { return 0.0; }
    void setParameterValue(int32_t, double) override {}
    std::string getParameterValueString(int32_t, double) override { return {}; }
    void setPerNoteControllerValue(uint8_t, uint8_t, double) override {}
    std::string getPerNoteControllerValueString(uint8_t, uint8_t, double) override { return {}; }

    bool hasUISupport() override { return false; }
    bool createUI(bool, void*, std::function<bool(uint32_t,uint32_t)>) override { return false; }
    void destroyUI() override {}
    bool showUI() override { return false; }
    void hideUI() override {}
    bool isUIVisible() const override { return false; }
    bool setUISize(uint32_t, uint32_t) override { return false; }
    bool getUISize(uint32_t&, uint32_t&) override { return false; }
    bool canUIResize() override { return false; }

    remidy::PluginParameterSupport* parameterSupport() override { return nullptr; }
};

// Minimal SequencerEngine implementation
class WebSequencerEngine : public SequencerEngine {
public:
    struct InstanceInfo { std::string name; std::string format; int32_t track{-1}; std::unique_ptr<StubPluginInstance> instance; };

    std::unordered_map<int32_t, InstanceInfo> instances_;

    // Unused interfaces satisfied with no-ops
    void performPluginScanning(bool) override {}
    PluginCatalog& catalog() override { return dummyCatalog_; }
    std::string getPluginName(int32_t instanceId) override {
        auto it = instances_.find(instanceId);
        return it == instances_.end() ? std::string{} : it->second.name;
    }
    SequenceProcessContext& data() override { return dummyContext_; }
    std::vector<AudioPluginTrack *>& tracks() const override { return const_cast<std::vector<AudioPluginTrack *>&>(dummyTracks_); }
    std::vector<TrackInfo> getTrackInfos() override { return {}; }
    void setDefaultChannels(uint32_t, uint32_t) override {}
    void addSimpleTrack(std::string&, std::string&, std::function<void(int32_t,int32_t,std::string)>) override {}
    void addPluginToTrack(int32_t, std::string&, std::string&, std::function<void(int32_t,int32_t,std::string)>) override {}
    void setPluginOutputHandler(int32_t, PluginOutputHandler) override {}
    void assignMidiDeviceToPlugin(int32_t, std::shared_ptr<MidiIODevice>) override {}
    void clearMidiDeviceFromPlugin(int32_t) override {}
    bool removePluginInstance(int32_t instanceId) override { return instances_.erase(instanceId) > 0; }
    uapmd_status_t processAudio(AudioProcessContext&) override { return 0; }
    bool isPlaybackActive() const override { return false; }
    void playbackPosition(int64_t) override {}
    int64_t playbackPosition() const override { return 0; }
    void startPlayback() override {}
    void stopPlayback() override {}
    void pausePlayback() override {}
    void resumePlayback() override {}
    void loadAudioFile(std::unique_ptr<AudioFileReader>) override {}
    void unloadAudioFile() override {}
    double audioFileDurationSeconds() const override { return 0.0; }
    void getInputSpectrum(float* out, int n) const override { if (out && n>0) for (int i=0;i<n;++i) out[i]=0.0f; }
    void getOutputSpectrum(float* out, int n) const override { if (out && n>0) for (int i=0;i<n;++i) out[i]=0.0f; }
    AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) override {
        auto it = instances_.find(instanceId);
        return it == instances_.end() ? nullptr : it->second.instance.get();
    }
    bool isPluginBypassed(int32_t) override { return false; }
    void setPluginBypassed(int32_t, bool) override {}
    std::optional<uint8_t> groupForInstance(int32_t) const override { return std::nullopt; }
    std::optional<int32_t> instanceForGroup(uint8_t) const override { return std::nullopt; }
    void enqueueUmp(int32_t, uapmd_ump_t*, size_t, uapmd_timestamp_t) override {}
    void sendNoteOn(int32_t, int32_t) override {}
    void sendNoteOff(int32_t, int32_t) override {}
    void sendPitchBend(int32_t, float) override {}
    void sendChannelPressure(int32_t, float) override {}
    void setParameterValue(int32_t, int32_t, double) override {}
    void registerParameterListener(int32_t, AudioPluginInstanceAPI*) override {}
    void unregisterParameterListener(int32_t) override {}
    std::vector<ParameterUpdate> getParameterUpdates(int32_t) override { return {}; }
    bool offlineRendering() const override { return false; }
    void offlineRendering(bool) override {}

private:
    PluginCatalog dummyCatalog_{}; // never used in web build; avoid calls to methods
    SequenceProcessContext dummyContext_{};
    mutable std::vector<AudioPluginTrack*> dummyTracks_{};
};

// Implement AudioPluginSequencer methods for web
AudioPluginSequencer::AudioPluginSequencer(size_t, size_t, int32_t, DeviceIODispatcher*)
    : buffer_size_in_frames(0), ump_buffer_size_in_bytes(0), sample_rate(0), dispatcher(nullptr) {
    sequencer = std::unique_ptr<SequencerEngine>(new WebSequencerEngine());
}

AudioPluginSequencer::~AudioPluginSequencer() = default;

std::vector<int32_t> AudioPluginSequencer::getInstanceIds() {
    auto* ws = dynamic_cast<WebSequencerEngine*>(sequencer.get());
    std::vector<int32_t> ids;
    if (ws) {
        ids.reserve(ws->instances_.size());
        for (auto& kv : ws->instances_) ids.push_back(kv.first);
    }
    return ids;
}

std::string AudioPluginSequencer::getPluginFormat(int32_t instanceId) {
    auto* ws = dynamic_cast<WebSequencerEngine*>(sequencer.get());
    if (!ws) return {};
    auto it = ws->instances_.find(instanceId);
    return it == ws->instances_.end() ? std::string{} : it->second.format;
}

int32_t AudioPluginSequencer::findTrackIndexForInstance(int32_t instanceId) const {
    auto* ws = dynamic_cast<WebSequencerEngine*>(sequencer.get());
    if (!ws) return -1;
    auto it = ws->instances_.find(instanceId);
    return it == ws->instances_.end() ? -1 : it->second.track;
}

uapmd_status_t AudioPluginSequencer::startAudio() { return 0; }
uapmd_status_t AudioPluginSequencer::stopAudio() { return 0; }
uapmd_status_t AudioPluginSequencer::isAudioPlaying() { return 0; }
int32_t AudioPluginSequencer::sampleRate() { return 48000; }
bool AudioPluginSequencer::sampleRate(int32_t) { return true; }
bool AudioPluginSequencer::reconfigureAudioDevice(int, int, uint32_t, uint32_t) { return true; }

// AppModel stub implementation
static std::unique_ptr<AppModel> g_model;

AppModel::AppModel() : sequencer_(new AudioPluginSequencer(256, 1024, 48000, nullptr)) {}

void AppModel::instantiate() { if (!g_model) g_model = std::unique_ptr<AppModel>(new AppModel()); }
AppModel& AppModel::instance() { return *g_model; }
void AppModel::cleanupInstance() { g_model.reset(); }

AudioPluginSequencer& AppModel::sequencer() { return *sequencer_; }

void AppModel::performPluginScanning(bool) {
    isScanning_ = false;
    for (auto& cb : scanningCompleted) cb(true, "");
}

void AppModel::createPluginInstanceAsync(const std::string& format,
                                         const std::string& pluginId,
                                         int32_t trackIndex,
                                         const PluginInstanceConfig&) {
    auto* ws = dynamic_cast<WebSequencerEngine*>(sequencer_->engine());
    if (!ws) return;
    int32_t id = nextInstanceId_++;
    WebSequencerEngine::InstanceInfo info;
    info.name = pluginId.empty() ? std::string("Plugin ") + std::to_string(id) : pluginId;
    info.format = format.empty() ? std::string("VST3") : format;
    info.track = trackIndex;
    info.instance = std::make_unique<StubPluginInstance>();
    ws->instances_[id] = std::move(info);
    for (auto& cb : instanceCreated) cb(PluginInstanceResult{ .instanceId = id, .pluginName = ws->instances_[id].name });
}

void AppModel::removePluginInstance(int32_t instanceId) {
    auto* ws = dynamic_cast<WebSequencerEngine*>(sequencer_->engine());
    if (ws) ws->instances_.erase(instanceId);
    for (auto& cb : instanceRemoved) cb(instanceId);
}

void AppModel::requestShowPluginUI(int32_t instanceId) {
    for (auto& cb : uiShowRequested) cb(instanceId);
}

}
