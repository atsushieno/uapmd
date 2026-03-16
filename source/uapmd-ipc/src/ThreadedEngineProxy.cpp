#include <uapmd-ipc/ThreadedEngineProxy.hpp>

namespace uapmd::ipc {

ThreadedEngineProxy::ThreadedEngineProxy(std::unique_ptr<SequencerEngine> engine)
    : engine_(std::move(engine)) {}

// ── Audio processing ─────────────────────────────────────────────────────

uapmd_status_t ThreadedEngineProxy::processAudio(AudioProcessContext& process) {
    drainCommandQueue();
    return engine_->processAudio(process);
}

void ThreadedEngineProxy::drainCommandQueue() {
    EngineCommand cmd;
    while (cmd_queue_.try_dequeue(cmd))
        applyCommand(cmd);
}

void ThreadedEngineProxy::applyCommand(EngineCommand& cmd) {
    std::visit([this](auto& c) { applyCommand(c); }, cmd);
}

// Per-command application (called from the audio thread during drainCommandQueue)

void ThreadedEngineProxy::applyCommand(AddTrackCmd& cmd) {
    auto index = engine_->addEmptyTrack();
    if (cmd.result)
        cmd.result->set_value(index);
}

void ThreadedEngineProxy::applyCommand(RemoveTrackCmd& cmd) {
    auto ok = engine_->removeTrack(cmd.trackIndex);
    if (cmd.result)
        cmd.result->set_value(ok);
}

void ThreadedEngineProxy::applyCommand(AddPluginCmd& cmd) {
    engine_->addPluginToTrack(cmd.trackIndex, cmd.format, cmd.pluginId, std::move(cmd.callback));
}

void ThreadedEngineProxy::applyCommand(RemovePluginCmd& cmd) {
    auto ok = engine_->removePluginInstance(cmd.instanceId);
    if (cmd.result)
        cmd.result->set_value(ok);
}

void ThreadedEngineProxy::applyCommand(SetInstanceGroupCmd& cmd) {
    auto ok = engine_->setInstanceGroup(cmd.instanceId, cmd.group);
    if (cmd.result)
        cmd.result->set_value(ok);
}

void ThreadedEngineProxy::applyCommand(StartPlaybackCmd&)   { engine_->startPlayback(); }
void ThreadedEngineProxy::applyCommand(StopPlaybackCmd&)    { engine_->stopPlayback(); }
void ThreadedEngineProxy::applyCommand(PausePlaybackCmd&)   { engine_->pausePlayback(); }
void ThreadedEngineProxy::applyCommand(ResumePlaybackCmd&)  { engine_->resumePlayback(); }

void ThreadedEngineProxy::applyCommand(SeekCmd& cmd) {
    engine_->playbackPosition(cmd.samples);
}

void ThreadedEngineProxy::applyCommand(SetDefaultChannelsCmd& cmd) {
    engine_->setDefaultChannels(cmd.inputChannels, cmd.outputChannels);
}

void ThreadedEngineProxy::applyCommand(CleanupEmptyTracksCmd&) {
    engine_->cleanupEmptyTracks();
}

// ── Transport (queued) ────────────────────────────────────────────────────

void ThreadedEngineProxy::startPlayback()   { cmd_queue_.emplace(StartPlaybackCmd{}); }
void ThreadedEngineProxy::stopPlayback()    { cmd_queue_.emplace(StopPlaybackCmd{}); }
void ThreadedEngineProxy::pausePlayback()   { cmd_queue_.emplace(PausePlaybackCmd{}); }
void ThreadedEngineProxy::resumePlayback()  { cmd_queue_.emplace(ResumePlaybackCmd{}); }

void ThreadedEngineProxy::playbackPosition(int64_t samples) {
    cmd_queue_.emplace(SeekCmd{samples});
}

// ── Configuration (queued) ────────────────────────────────────────────────

void ThreadedEngineProxy::setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) {
    cmd_queue_.emplace(SetDefaultChannelsCmd{inputChannels, outputChannels});
}

void ThreadedEngineProxy::cleanupEmptyTracks() {
    cmd_queue_.emplace(CleanupEmptyTracksCmd{});
}

// ── Structural mutations (direct, Phase 1) ────────────────────────────────

uapmd_track_index_t ThreadedEngineProxy::addEmptyTrack() {
    return engine_->addEmptyTrack();
}

void ThreadedEngineProxy::addPluginToTrack(uapmd_track_index_t trackIndex, std::string& format,
                                           std::string& pluginId,
                                           std::function<void(int32_t, uapmd_track_index_t, std::string)> callback) {
    engine_->addPluginToTrack(trackIndex, format, pluginId, std::move(callback));
}

bool ThreadedEngineProxy::removePluginInstance(int32_t instanceId) {
    return engine_->removePluginInstance(instanceId);
}

bool ThreadedEngineProxy::removeTrack(uapmd_track_index_t trackIndex) {
    return engine_->removeTrack(trackIndex);
}

uint8_t ThreadedEngineProxy::getInstanceGroup(int32_t instanceId) const {
    return engine_->getInstanceGroup(instanceId);
}

bool ThreadedEngineProxy::setInstanceGroup(int32_t instanceId, uint8_t group) {
    return engine_->setInstanceGroup(instanceId, group);
}

// ── Forwarded queries and already-thread-safe operations ──────────────────

void ThreadedEngineProxy::enqueueUmp(int32_t instanceId, uapmd_ump_t* ump,
                                     size_t sizeInBytes, uapmd_timestamp_t timestamp) {
    engine_->enqueueUmp(instanceId, ump, sizeInBytes, timestamp);
}

AudioPluginHostingAPI* ThreadedEngineProxy::pluginHost() { return engine_->pluginHost(); }

AudioPluginInstanceAPI* ThreadedEngineProxy::getPluginInstance(int32_t instanceId) {
    return engine_->getPluginInstance(instanceId);
}

UapmdFunctionBlockManager* ThreadedEngineProxy::functionBlockManager() {
    return engine_->functionBlockManager();
}

int32_t ThreadedEngineProxy::findTrackIndexForInstance(int32_t instanceId) const {
    return engine_->findTrackIndexForInstance(instanceId);
}

std::vector<SequencerTrack*>& ThreadedEngineProxy::tracks() const {
    return engine_->tracks();
}

SequencerTrack* ThreadedEngineProxy::masterTrack() { return engine_->masterTrack(); }

bool ThreadedEngineProxy::offlineRendering() const { return engine_->offlineRendering(); }
void ThreadedEngineProxy::offlineRendering(bool enabled) { engine_->offlineRendering(enabled); }
void ThreadedEngineProxy::setEngineActive(bool active) { engine_->setEngineActive(active); }

void ThreadedEngineProxy::setAudioPreprocessCallback(AudioPreprocessCallback callback) {
    engine_->setAudioPreprocessCallback(std::move(callback));
}

SequenceProcessContext& ThreadedEngineProxy::data() { return engine_->data(); }

bool ThreadedEngineProxy::isPlaybackActive() const { return engine_->isPlaybackActive(); }
int64_t ThreadedEngineProxy::playbackPosition() const { return engine_->playbackPosition(); }

void ThreadedEngineProxy::getInputSpectrum(float* outSpectrum, int numBars) const {
    engine_->getInputSpectrum(outSpectrum, numBars);
}

void ThreadedEngineProxy::getOutputSpectrum(float* outSpectrum, int numBars) const {
    engine_->getOutputSpectrum(outSpectrum, numBars);
}

void ThreadedEngineProxy::sendNoteOn(int32_t instanceId, int32_t note) {
    engine_->sendNoteOn(instanceId, note);
}

void ThreadedEngineProxy::sendNoteOff(int32_t instanceId, int32_t note) {
    engine_->sendNoteOff(instanceId, note);
}

void ThreadedEngineProxy::sendPitchBend(int32_t instanceId, float normalizedValue) {
    engine_->sendPitchBend(instanceId, normalizedValue);
}

void ThreadedEngineProxy::sendChannelPressure(int32_t instanceId, float pressure) {
    engine_->sendChannelPressure(instanceId, pressure);
}

void ThreadedEngineProxy::setParameterValue(int32_t instanceId, int32_t index, double value) {
    engine_->setParameterValue(instanceId, index, value);
}

TimelineFacade& ThreadedEngineProxy::timeline() { return engine_->timeline(); }

} // namespace uapmd::ipc
