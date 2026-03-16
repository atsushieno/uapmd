#pragma once

#include "EngineMessages.hpp"
#include <readerwriterqueue.h>
#include <memory>

namespace uapmd::ipc {

// A SequencerEngine proxy that queues transport/configuration commands so
// they are applied at the start of the next processAudio() call (clean
// audio-buffer boundary).  Structural mutations (add/remove track/plugin)
// are forwarded directly to the wrapped engine in Phase 1; Phase 2 will
// route them through the queue with safe-point semantics.
//
// This class is the universal baseline: it compiles and runs on all
// platforms (desktop, iOS, Android, WASM).  RemoteEngineProxy (desktop-
// only, Phase 2) sends the same EngineCommand messages over a socket
// instead of applying them in-process.
class ThreadedEngineProxy final : public SequencerEngine {
    std::unique_ptr<SequencerEngine> engine_;
    moodycamel::ReaderWriterQueue<EngineCommand> cmd_queue_{64};

    void drainCommandQueue();
    void applyCommand(EngineCommand& cmd);

    // Per-type overloads called from the audio thread via applyCommand().
    void applyCommand(AddTrackCmd& cmd);
    void applyCommand(RemoveTrackCmd& cmd);
    void applyCommand(AddPluginCmd& cmd);
    void applyCommand(RemovePluginCmd& cmd);
    void applyCommand(SetInstanceGroupCmd& cmd);
    void applyCommand(StartPlaybackCmd& cmd);
    void applyCommand(StopPlaybackCmd& cmd);
    void applyCommand(PausePlaybackCmd& cmd);
    void applyCommand(ResumePlaybackCmd& cmd);
    void applyCommand(SeekCmd& cmd);
    void applyCommand(SetDefaultChannelsCmd& cmd);
    void applyCommand(CleanupEmptyTracksCmd& cmd);

public:
    explicit ThreadedEngineProxy(std::unique_ptr<SequencerEngine> engine);
    ~ThreadedEngineProxy() override = default;

    // ── Audio processing ─────────────────────────────────────────────────
    // Drains the command queue before delegating to the wrapped engine so
    // transport changes take effect at a clean buffer boundary.
    uapmd_status_t processAudio(AudioProcessContext& process) override;

    // ── Transport (queued) ───────────────────────────────────────────────
    void startPlayback() override;
    void stopPlayback() override;
    void pausePlayback() override;
    void resumePlayback() override;
    void playbackPosition(int64_t samples) override;

    // ── Configuration (queued) ───────────────────────────────────────────
    void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
    void cleanupEmptyTracks() override;

    // ── Structural mutations (direct, Phase 1) ───────────────────────────
    uapmd_track_index_t addEmptyTrack() override;
    void addPluginToTrack(uapmd_track_index_t trackIndex, std::string& format, std::string& pluginId,
                          std::function<void(int32_t, uapmd_track_index_t, std::string)> callback) override;
    bool removePluginInstance(int32_t instanceId) override;
    bool removeTrack(uapmd_track_index_t trackIndex) override;
    uint8_t getInstanceGroup(int32_t instanceId) const override;
    bool    setInstanceGroup(int32_t instanceId, uint8_t group) override;

    // ── Forwarded queries and already-thread-safe operations ─────────────
    void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes,
                    uapmd_timestamp_t timestamp) override;

    AudioPluginHostingAPI* pluginHost() override;
    AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) override;
    UapmdFunctionBlockManager* functionBlockManager() override;
    int32_t findTrackIndexForInstance(int32_t instanceId) const override;
    std::vector<SequencerTrack*>& tracks() const override;
    SequencerTrack* masterTrack() override;

    bool offlineRendering() const override;
    void offlineRendering(bool enabled) override;
    void setEngineActive(bool active) override;

    void setAudioPreprocessCallback(AudioPreprocessCallback callback) override;

    SequenceProcessContext& data() override;

    bool isPlaybackActive() const override;
    int64_t playbackPosition() const override;

    void getInputSpectrum(float* outSpectrum, int numBars) const override;
    void getOutputSpectrum(float* outSpectrum, int numBars) const override;

    void sendNoteOn(int32_t instanceId, int32_t note) override;
    void sendNoteOff(int32_t instanceId, int32_t note) override;
    void sendPitchBend(int32_t instanceId, float normalizedValue) override;
    void sendChannelPressure(int32_t instanceId, float pressure) override;
    void setParameterValue(int32_t instanceId, int32_t index, double value) override;

    TimelineFacade& timeline() override;
};

} // namespace uapmd::ipc
