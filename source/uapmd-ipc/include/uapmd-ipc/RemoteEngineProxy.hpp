#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

#include "SharedAudioRegion.hpp"
#include "platform/SharedMemory.hpp"

namespace uapmd::ipc {

// Out-of-process SequencerEngine proxy.
//
// Audio I/O round-trip: shared memory with atomic sequence counters.
// Control commands    : AF_UNIX socket with binary framing (IpcProtocol.hpp).
//
// Phase 2 limitations (noted inline):
//   • pluginHost(), getPluginInstance(), functionBlockManager() → nullptr
//   • tracks() → always returns an empty vector
//   • timeline() → returns a no-op stub (clip playback not yet wired cross-process)
//   • setAudioPreprocessCallback() → ignored (engine handles its own clips)
//   • data() → returns a local SequenceProcessContext (not mirrored)
class RemoteEngineProxy final : public SequencerEngine {
public:
    // Spawn a child process and connect shared memory + socket.
    // executablePath is usually argv[0].  Returns nullptr on failure.
    static std::unique_ptr<RemoteEngineProxy> launch(
        const std::string& executablePath,
        int32_t            sampleRate,
        size_t             audioBufferSizeInFrames,
        size_t             umpBufferSizeInBytes);

    ~RemoteEngineProxy() override;

    // True while the engine child process is responsive.
    bool isEngineAlive() const {
        return alive_.load(std::memory_order_acquire);
    }

    // ── SequencerEngine interface ──────────────────────────────────────────

    uapmd_status_t processAudio(AudioProcessContext& process) override;

    void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump,
                    size_t sizeInBytes, uapmd_timestamp_t timestamp) override;

    // These require deep cross-process object access — stubbed for Phase 2.
    AudioPluginHostingAPI*  pluginHost()                        override { return nullptr; }
    AudioPluginInstanceAPI* getPluginInstance(int32_t)          override { return nullptr; }
    UapmdFunctionBlockManager* functionBlockManager()           override { return nullptr; }
    int32_t findTrackIndexForInstance(int32_t) const            override { return -1; }
    std::vector<SequencerTrack*>& tracks()                const override;
    SequencerTrack* masterTrack()                               override { return nullptr; }

    uapmd_track_index_t addEmptyTrack() override;
    void addPluginToTrack(uapmd_track_index_t trackIndex, std::string& format,
                          std::string& pluginId,
                          std::function<void(int32_t, uapmd_track_index_t, std::string)> callback) override;
    bool removePluginInstance(int32_t instanceId) override;
    bool removeTrack(uapmd_track_index_t trackIndex) override;

    uint8_t getInstanceGroup(int32_t instanceId) const override;
    bool    setInstanceGroup(int32_t instanceId, uint8_t group) override;
    void    cleanupEmptyTracks() override;

    void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;

    bool offlineRendering() const override;
    void offlineRendering(bool enabled) override;

    void setEngineActive(bool active) override;

    // Ignored in Phase 2: the engine process handles its own clip scheduling.
    void setAudioPreprocessCallback(AudioPreprocessCallback) override {}

    // Local stub — not mirrored from the engine process.
    SequenceProcessContext& data() override;

    bool    isPlaybackActive() const override;
    void    playbackPosition(int64_t samples) override;
    int64_t playbackPosition() const override;
    void    startPlayback()  override;
    void    stopPlayback()   override;
    void    pausePlayback()  override;
    void    resumePlayback() override;

    // Spectra are not transmitted cross-process in Phase 2; buffers are zeroed.
    void getInputSpectrum(float* outSpectrum, int numBars)  const override;
    void getOutputSpectrum(float* outSpectrum, int numBars) const override;

    void sendNoteOn(int32_t instanceId, int32_t note) override;
    void sendNoteOff(int32_t instanceId, int32_t note) override;
    void sendPitchBend(int32_t instanceId, float normalizedValue) override;
    void sendChannelPressure(int32_t instanceId, float pressure) override;
    void setParameterValue(int32_t instanceId, int32_t index, double value) override;

    // Returns a no-op stub.  Clip management over IPC is Phase 3 work.
    TimelineFacade& timeline() override;

private:
    RemoteEngineProxy() = default;

    bool init(const std::string& executablePath, int32_t sampleRate,
              size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes);
    void shutdown();
    void readerLoop();

    // Send a fire-and-forget command (no response expected).
    bool sendFire(uint32_t type, const std::vector<uint8_t>& payload);

    // Send a command and block until a matching response arrives (or timeout).
    // Returns defaultValue on timeout / engine death.
    int32_t callSyncI32(uint32_t type, uint64_t reqId,
                         const std::vector<uint8_t>& payload,
                         std::shared_ptr<std::promise<int32_t>> promise);
    bool callSyncBool(uint32_t type, uint64_t reqId,
                      const std::vector<uint8_t>& payload,
                      std::shared_ptr<std::promise<bool>> promise);
    uint8_t callSyncU8(uint32_t type, uint64_t reqId,
                       const std::vector<uint8_t>& payload,
                       std::shared_ptr<std::promise<uint8_t>> promise);

    uint64_t nextReqId() {
        return next_req_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // Shared memory
    std::unique_ptr<SharedMemory> shm_;
    SharedAudioRegion*            region_{nullptr};
    uint64_t                      expected_engine_seq_{0};

    // Control socket (client side)
    int sock_fd_{-1};

    // Child process handle
#ifdef _WIN32
    void* child_proc_{nullptr};
    void* child_thread_{nullptr};
#else
    int child_pid_{-1};
#endif

    // Reader thread
    std::thread       reader_thread_;
    std::atomic<bool> alive_{false};
    std::atomic<bool> running_{false};

    // Pending synchronous requests keyed by req_id.
    // One of the promise pointers is non-null depending on the return type.
    struct PendingRequest {
        std::shared_ptr<std::promise<int32_t>> p_i32;
        std::shared_ptr<std::promise<bool>>    p_bool;
        std::shared_ptr<std::promise<uint8_t>> p_u8;
    };
    mutable std::mutex                              pending_mutex_;
    std::unordered_map<uint64_t, PendingRequest>    pending_;

    // Async callbacks for addPluginToTrack (fired when engine is done loading).
    std::mutex                                                             callbacks_mutex_;
    std::unordered_map<uint64_t,
        std::function<void(int32_t, int32_t, std::string)>>               callbacks_;

    std::atomic<uint64_t> next_req_id_{1};

    // Serialises writes to sock_fd_ (reads happen on reader_thread_ only).
    std::mutex write_mutex_;

    // Stubs for methods that can't be mirrored in Phase 2.
    mutable std::vector<SequencerTrack*> dummy_tracks_;
    std::unique_ptr<SequenceProcessContext> dummy_data_;
    std::unique_ptr<TimelineFacade>         dummy_timeline_;

    // Cached playback state updated by processAudio (for isPlaybackActive /
    // playbackPosition queries from the UI thread).
    mutable std::atomic<bool>    playback_active_{false};
    mutable std::atomic<int64_t> playback_pos_{0};
    mutable std::atomic<bool>    offline_rendering_{false};
};

} // namespace uapmd::ipc
