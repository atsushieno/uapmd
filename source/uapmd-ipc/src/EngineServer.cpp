#include <uapmd-ipc/EngineServer.hpp>
#include <uapmd-ipc/IpcProtocol.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <afunix.h>
    static bool initWinsock() {
        WSADATA w; return ::WSAStartup(MAKEWORD(2,2), &w) == 0;
    }
    static struct WsaInit { WsaInit() { initWinsock(); } } _wsaInit;
    using socket_t = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCK(s) ::closesocket(s)
#else
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
    using socket_t = int;
    #define INVALID_SOCK (-1)
    #define CLOSE_SOCK(s) ::close(s)
#endif

namespace uapmd::ipc {

EngineServer::EngineServer(const std::string& shmName,
                           size_t             shmSize,
                           const std::string& socketPath,
                           int32_t            sampleRate,
                           size_t             audioBufferSizeInFrames,
                           size_t             umpBufferSizeInBytes)
    : sample_rate_(sampleRate),
      audio_buffer_size_(audioBufferSizeInFrames),
      ump_buffer_size_(umpBufferSizeInBytes),
      audio_ctx_(master_ctx_, static_cast<uint32_t>(umpBufferSizeInBytes))
{
    // Map the shared memory segment created by the host
    shm_ = SharedMemory::open(shmName, shmSize);
    if (!shm_) {
        std::cerr << "[EngineServer] failed to open shared memory '" << shmName << "'\n";
        return;
    }
    region_ = static_cast<SharedAudioRegion*>(shm_->data());

    // Configure audio context (stereo main bus by default; host will send SetDefaultChannels)
    master_ctx_.sampleRate(sampleRate);
    audio_ctx_.configureMainBus(2, 2, audioBufferSizeInFrames);

    // Connect to the host's Unix-domain socket
    socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) {
        std::cerr << "[EngineServer] socket() failed\n";
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    // The host creates the socket before spawning us, but accept() may not
    // have been called yet — retry briefly.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int rc = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (rc != 0) {
        std::cerr << "[EngineServer] connect to '" << socketPath << "' failed\n";
        CLOSE_SOCK(fd);
        return;
    }
    sock_fd_ = static_cast<int>(fd);

    // Create the real audio engine
    engine_ = SequencerEngine::create(sampleRate, audioBufferSizeInFrames, umpBufferSizeInBytes);
    if (!engine_) {
        std::cerr << "[EngineServer] SequencerEngine::create failed\n";
        return;
    }
    engine_->setEngineActive(true);

    running_ = true;
}

EngineServer::~EngineServer() {
    running_ = false;
    if (audio_thread_.joinable())
        audio_thread_.join();
    if (sock_fd_ >= 0) {
        CLOSE_SOCK(sock_fd_);
        sock_fd_ = -1;
    }
}

// ── Audio thread: drives the shared-memory round-trip ────────────────────

void EngineServer::audioLoop() {
    uint64_t last_seq = 0;
    auto& reg = *region_;

    while (running_) {
        // Spin-wait for host to signal new input (with a brief yield to avoid
        // burning 100% CPU on the child while idle).
        uint64_t cur;
        int spin = 0;
        while ((cur = reg.host_seq.load(std::memory_order_acquire)) <= last_seq) {
            if (!running_) return;
            if (++spin > 4096) {
                std::this_thread::yield();
                spin = 0;
            }
        }

        // ── Sync MasterContext from shared region ────────────────────────
        master_ctx_.sampleRate(static_cast<int32_t>(reg.sample_rate));
        master_ctx_.isPlaying(reg.is_playing);
        master_ctx_.playbackPositionSamples(reg.playback_position_samples);
        master_ctx_.tempo(reg.tempo);
        master_ctx_.timeSignatureNumerator(reg.time_sig_num);
        master_ctx_.timeSignatureDenominator(reg.time_sig_den);

        const uint32_t nFrames = reg.frame_count;
        const uint32_t nIn     = std::min(reg.in_channels,  kSharedAudioMaxChannels);
        const uint32_t nOut    = std::min(reg.out_channels, kSharedAudioMaxChannels);

        // Reconfigure bus if channel layout changed
        if (static_cast<uint32_t>(audio_ctx_.inputChannelCount(0))  != nIn ||
            static_cast<uint32_t>(audio_ctx_.outputChannelCount(0)) != nOut)
            audio_ctx_.configureMainBus(static_cast<int32_t>(nIn),
                                        static_cast<int32_t>(nOut),
                                        audio_buffer_size_);
        audio_ctx_.frameCount(static_cast<int32_t>(nFrames));

        // ── Copy input audio from shared region ──────────────────────────
        for (uint32_t ch = 0; ch < nIn; ++ch) {
            float* dst = audio_ctx_.getFloatInBuffer(0, ch);
            if (dst)
                std::memcpy(dst, reg.audio_in[ch], nFrames * sizeof(float));
        }

        // ── Copy input events from shared region ─────────────────────────
        {
            auto& evIn = audio_ctx_.eventIn();
            uint32_t evSize = std::min(reg.event_in_size, kSharedEventMaxBytes);
            if (evSize > 0)
                std::memcpy(evIn.getMessages(), reg.event_in_data, evSize);
            evIn.position(evSize);
        }

        // ── Process audio ────────────────────────────────────────────────
        engine_->processAudio(audio_ctx_);

        // ── Copy output audio back to shared region ──────────────────────
        for (uint32_t ch = 0; ch < nOut; ++ch) {
            const float* src = audio_ctx_.getFloatOutBuffer(0, ch);
            if (src)
                std::memcpy(reg.audio_out[ch], src, nFrames * sizeof(float));
            else
                std::memset(reg.audio_out[ch], 0,   nFrames * sizeof(float));
        }

        // ── Copy output events back ──────────────────────────────────────
        {
            auto& evOut = audio_ctx_.eventOut();
            uint32_t evSize = std::min(static_cast<uint32_t>(evOut.position()), kSharedEventMaxBytes);
            reg.event_out_size = evSize;
            if (evSize > 0)
                std::memcpy(reg.event_out_data, evOut.getMessages(), evSize);
        }

        // ── Update playback position so host can read it ─────────────────
        reg.playback_position_samples =
            master_ctx_.playbackPositionSamples();

        // ── Signal host: output is ready ─────────────────────────────────
        last_seq = cur;
        reg.engine_seq.store(cur, std::memory_order_release);
    }
}

// ── Control command dispatcher ────────────────────────────────────────────

bool EngineServer::sendReply(uint32_t type, uint64_t reqId,
                              const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lk(write_mutex_);
    return ipcSendMsg(sock_fd_, static_cast<IpcMsgType>(type), reqId, payload);
}

void EngineServer::dispatchCommand(uint32_t type, uint64_t reqId,
                                   const uint8_t* payload, size_t payloadSize)
{
    IpcReader r{payload, payloadSize};
    const auto msgType = static_cast<IpcMsgType>(type);

    switch (msgType) {
    case IpcMsgType::SetDefaultChannels: {
        uint32_t inCh  = r.u32();
        uint32_t outCh = r.u32();
        engine_->setDefaultChannels(inCh, outCh);
        audio_ctx_.configureMainBus(static_cast<int32_t>(inCh),
                                    static_cast<int32_t>(outCh),
                                    audio_buffer_size_);
        break;
    }
    case IpcMsgType::SetEngineActive:
        engine_->setEngineActive(r.bln());
        break;

    case IpcMsgType::OfflineRenderingSet:
        engine_->offlineRendering(r.bln());
        break;

    case IpcMsgType::AddEmptyTrack: {
        int32_t idx = engine_->addEmptyTrack();
        std::vector<uint8_t> rp;
        writeI32(rp, idx);
        sendReply(static_cast<uint32_t>(IpcMsgType::AddEmptyTrackResult), reqId, rp);
        break;
    }
    case IpcMsgType::AddPlugin: {
        int32_t trackIdx = r.i32();
        std::string format   = r.str();
        std::string pluginId = r.str();
        uint64_t cbId        = r.u64();
        engine_->addPluginToTrack(trackIdx, format, pluginId,
            [this, cbId](int32_t instId, int32_t trackIdx2, std::string error) {
                std::vector<uint8_t> rp;
                writeU64(rp, cbId);
                writeI32(rp, instId);
                writeI32(rp, trackIdx2);
                writeStr(rp, error);
                sendReply(static_cast<uint32_t>(IpcMsgType::AddPluginCallback), 0, rp);
            });
        break;
    }
    case IpcMsgType::RemovePlugin: {
        bool ok = engine_->removePluginInstance(r.i32());
        std::vector<uint8_t> rp;
        writeBool(rp, ok);
        sendReply(static_cast<uint32_t>(IpcMsgType::BoolResult), reqId, rp);
        break;
    }
    case IpcMsgType::RemoveTrack: {
        bool ok = engine_->removeTrack(r.i32());
        std::vector<uint8_t> rp;
        writeBool(rp, ok);
        sendReply(static_cast<uint32_t>(IpcMsgType::BoolResult), reqId, rp);
        break;
    }
    case IpcMsgType::StartPlayback:   engine_->startPlayback();   break;
    case IpcMsgType::StopPlayback:    engine_->stopPlayback();    break;
    case IpcMsgType::PausePlayback:   engine_->pausePlayback();   break;
    case IpcMsgType::ResumePlayback:  engine_->resumePlayback();  break;
    case IpcMsgType::Seek:            engine_->playbackPosition(r.i64()); break;
    case IpcMsgType::CleanupEmptyTracks: engine_->cleanupEmptyTracks(); break;

    case IpcMsgType::EnqueueUmp: {
        int32_t  instId    = r.i32();
        auto     ts        = static_cast<uapmd_timestamp_t>(r.u64());
        uint32_t sz        = r.u32();
        const uint8_t* ump = r.bytes(sz);
        engine_->enqueueUmp(instId, reinterpret_cast<uapmd_ump_t*>(const_cast<uint8_t*>(ump)), sz, ts);
        break;
    }
    case IpcMsgType::SendNoteOn:
        engine_->sendNoteOn(r.i32(), r.i32());
        break;
    case IpcMsgType::SendNoteOff:
        engine_->sendNoteOff(r.i32(), r.i32());
        break;
    case IpcMsgType::SendPitchBend: {
        int32_t instId = r.i32();
        engine_->sendPitchBend(instId, r.f32());
        break;
    }
    case IpcMsgType::SendChannelPressure: {
        int32_t instId = r.i32();
        engine_->sendChannelPressure(instId, r.f32());
        break;
    }
    case IpcMsgType::SetParameterValue: {
        int32_t instId = r.i32();
        int32_t idx    = r.i32();
        engine_->setParameterValue(instId, idx, r.f64());
        break;
    }
    case IpcMsgType::GetInstanceGroup: {
        uint8_t grp = engine_->getInstanceGroup(r.i32());
        std::vector<uint8_t> rp;
        writeU8(rp, grp);
        sendReply(static_cast<uint32_t>(IpcMsgType::GetInstanceGroupResult), reqId, rp);
        break;
    }
    case IpcMsgType::SetInstanceGroup: {
        int32_t instId = r.i32();
        bool ok = engine_->setInstanceGroup(instId, r.u8());
        std::vector<uint8_t> rp;
        writeBool(rp, ok);
        sendReply(static_cast<uint32_t>(IpcMsgType::BoolResult), reqId, rp);
        break;
    }
    case IpcMsgType::Shutdown:
        running_ = false;
        break;
    default:
        break;
    }
}

// ── Main run loop ─────────────────────────────────────────────────────────

void EngineServer::run() {
    if (!running_ || sock_fd_ < 0) return;

    // Start audio thread
    audio_thread_ = std::thread([this]{ audioLoop(); });

    // Control loop: read commands from host
    while (running_) {
        IpcHeader hdr{};
        std::vector<uint8_t> payload;
        if (!ipcRecvMsg(sock_fd_, hdr, payload)) {
            // Host closed the connection
            break;
        }
        dispatchCommand(hdr.type, hdr.req_id,
                        payload.data(), payload.size());
    }

    running_ = false;
    if (audio_thread_.joinable())
        audio_thread_.join();
}

} // namespace uapmd::ipc
