#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

#include "SharedAudioRegion.hpp"
#include "platform/SharedMemory.hpp"

namespace uapmd::ipc {

// Runs inside the engine child process.
//
// Connects to the parent's Unix-domain control socket, maps the shared
// audio region, creates a SequencerEngine, and drives the audio round-trip
// from a dedicated audio thread.
//
// Typical usage (from main_common.cpp when --engine-mode is detected):
//
//   EngineServer server(shmName, shmSize, socketPath,
//                       sampleRate, audioBufferSize, umpBufferSize);
//   server.run();   // blocks until parent closes the socket or sends Shutdown
class EngineServer {
public:
    EngineServer(const std::string& shmName,
                 size_t             shmSize,
                 const std::string& socketPath,
                 int32_t            sampleRate,
                 size_t             audioBufferSizeInFrames,
                 size_t             umpBufferSizeInBytes);
    ~EngineServer();

    // Blocks until the control socket closes or a Shutdown message is received.
    void run();

private:
    void audioLoop();
    void dispatchCommand(uint32_t type, uint64_t reqId,
                         const uint8_t* payload, size_t payloadSize);
    bool sendReply(uint32_t type, uint64_t reqId, const std::vector<uint8_t>& payload);

    std::unique_ptr<SharedMemory> shm_;
    SharedAudioRegion*            region_{nullptr};
    int                           sock_fd_{-1};

    int32_t sample_rate_;
    size_t  audio_buffer_size_;
    size_t  ump_buffer_size_;

    std::unique_ptr<SequencerEngine> engine_;

    // Audio processing context owned by the server
    remidy::MasterContext    master_ctx_;
    remidy::AudioProcessContext audio_ctx_;

    // Audio thread that watches host_seq and calls engine_->processAudio()
    std::thread       audio_thread_;
    std::atomic<bool> running_{false};

    // Serialises socket writes (sendReply may be called from the plugin-load
    // callback thread in addition to the control loop thread).
    std::mutex write_mutex_;
};

} // namespace uapmd::ipc
