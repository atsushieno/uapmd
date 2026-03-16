#include <uapmd-ipc/RemoteEngineProxy.hpp>
#include <uapmd-ipc/IpcProtocol.hpp>

#include <chrono>
#include <cstring>
#ifndef _WIN32
    #include <sys/mman.h>   // shm_unlink
#endif
#include <iostream>
#include <stdexcept>
#include <thread>

// ── Platform-specific process spawn + socket setup ────────────────────────
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <afunix.h>        // AF_UNIX on Windows 10+
    #include <process.h>
    #define getpid() static_cast<int>(GetCurrentProcessId())
    static bool initWinsock() {
        WSADATA w;
        return ::WSAStartup(MAKEWORD(2,2), &w) == 0;
    }
    static struct WinsockInit { WinsockInit() { initWinsock(); } } _wsaInit;
    using socket_t = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCK(s) ::closesocket(s)
    static std::string makeTempPath(int pid) {
        char tmp[MAX_PATH + 1];
        ::GetTempPathA(sizeof(tmp) - 1, tmp);
        return std::string(tmp) + "uapmd-" + std::to_string(pid) + ".sock";
    }
    static std::string makeShmName(int pid) {
        return "Local\\uapmd-" + std::to_string(pid);
    }
#else
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <signal.h>
    #include <fcntl.h>
    using socket_t = int;
    #define INVALID_SOCK (-1)
    #define CLOSE_SOCK(s) ::close(s)
    static std::string makeTempPath(int pid) {
        return "/tmp/uapmd-" + std::to_string(pid) + ".sock";
    }
    static std::string makeShmName(int pid) {
        return "/uapmd-" + std::to_string(pid);
    }
#endif

// NullTimelineFacade — returned by RemoteEngineProxy::timeline() in Phase 2.
// All clip operations are no-ops; the engine process manages its own timeline.
namespace {
class NullTimelineFacade final : public uapmd::TimelineFacade {
    uapmd::TimelineState state_{};
public:
    uapmd::TimelineState& state() override { return state_; }
    std::vector<uapmd::TimelineTrack*> tracks() override { return {}; }

    ClipAddResult addClipToTrack(int32_t, const uapmd::TimelinePosition&,
        std::unique_ptr<uapmd::AudioFileReader>, const std::string&) override {
        return {-1, -1, false, "not supported in isolated engine mode"};
    }
    ClipAddResult addMidiClipToTrack(int32_t, const uapmd::TimelinePosition&,
        const std::string&, bool) override {
        return {-1, -1, false, "not supported in isolated engine mode"};
    }
    ClipAddResult addMidiClipToTrack(int32_t, const uapmd::TimelinePosition&,
        std::vector<uapmd_ump_t>, std::vector<uint64_t>, uint32_t,
        double, std::vector<uapmd::MidiTempoChange>,
        std::vector<uapmd::MidiTimeSignatureChange>,
        const std::string&, bool) override {
        return {-1, -1, false, "not supported in isolated engine mode"};
    }
    bool removeClipFromTrack(int32_t, int32_t) override { return false; }
    ProjectResult loadProject(const std::filesystem::path&) override {
        return {false, "not supported in isolated engine mode"};
    }
    MasterTrackSnapshot buildMasterTrackSnapshot() override { return {}; }
    ContentBounds calculateContentBounds() const override { return {}; }
    void processTracksAudio(uapmd::AudioProcessContext&) override {}
    void onTrackAdded(uint32_t, double, uint32_t) override {}
    void onTrackRemoved(size_t) override {}
};
} // anonymous namespace

namespace uapmd::ipc {

// ── Socket helpers ────────────────────────────────────────────────────────

static bool setSocketBlocking(socket_t fd, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

// ── Factory: spawn child + connect SHM + socket ───────────────────────────

std::unique_ptr<RemoteEngineProxy> RemoteEngineProxy::launch(
    const std::string& executablePath,
    int32_t            sampleRate,
    size_t             audioBufferSizeInFrames,
    size_t             umpBufferSizeInBytes)
{
    auto proxy = std::unique_ptr<RemoteEngineProxy>(new RemoteEngineProxy());
    if (!proxy->init(executablePath, sampleRate, audioBufferSizeInFrames, umpBufferSizeInBytes))
        return nullptr;
    return proxy;
}

bool RemoteEngineProxy::init(const std::string& executablePath,
                              int32_t sampleRate,
                              size_t audioBufferSizeInFrames,
                              size_t umpBufferSizeInBytes)
{
    const int pid         = getpid();
    const std::string shmName    = makeShmName(pid);
    const std::string socketPath = makeTempPath(pid);
    const size_t shmSize = sizeof(SharedAudioRegion);

    // Create shared memory segment
    shm_ = SharedMemory::create(shmName, shmSize);
    if (!shm_) {
        std::cerr << "[RemoteEngineProxy] failed to create shared memory\n";
        return false;
    }
    region_ = new (shm_->data()) SharedAudioRegion{};

    // Create a listening Unix domain socket
    socket_t server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        std::cerr << "[RemoteEngineProxy] failed to create server socket\n";
        return false;
    }
    ::unlink(socketPath.c_str());  // remove stale socket file if any

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(server_fd, 1) != 0) {
        std::cerr << "[RemoteEngineProxy] bind/listen failed\n";
        CLOSE_SOCK(server_fd);
        return false;
    }

    // Build child process command line:
    // <exe> --engine-mode <shmName> <shmSize> <socketPath> <sampleRate> <bufferSize> <umpSize>
    const std::string cmdLine =
        "\"" + executablePath + "\" --engine-mode " +
        shmName + " " + std::to_string(shmSize) + " " +
        socketPath + " " + std::to_string(sampleRate) + " " +
        std::to_string(audioBufferSizeInFrames) + " " +
        std::to_string(umpBufferSizeInBytes);

#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdLine; // CreateProcessA may modify this
    if (!::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                          FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::cerr << "[RemoteEngineProxy] CreateProcess failed\n";
        CLOSE_SOCK(server_fd);
        return false;
    }
    child_proc_   = pi.hProcess;
    child_thread_ = pi.hThread;
#else
    pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "[RemoteEngineProxy] fork failed\n";
        CLOSE_SOCK(server_fd);
        return false;
    }
    if (child == 0) {
        // Child: exec the same binary with engine-mode arguments
        execlp(executablePath.c_str(), executablePath.c_str(),
               "--engine-mode",
               shmName.c_str(),
               std::to_string(shmSize).c_str(),
               socketPath.c_str(),
               std::to_string(sampleRate).c_str(),
               std::to_string(audioBufferSizeInFrames).c_str(),
               std::to_string(umpBufferSizeInBytes).c_str(),
               nullptr);
        std::cerr << "[engine-child] exec failed\n";
        _exit(1);
    }
    child_pid_ = child;
#endif

    // Accept the child's connection (with a 5-second timeout)
    setSocketBlocking(server_fd, false);
    socket_t client_fd = INVALID_SOCK;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd != INVALID_SOCK)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CLOSE_SOCK(server_fd);
    ::unlink(socketPath.c_str());

    if (client_fd == INVALID_SOCK) {
        std::cerr << "[RemoteEngineProxy] accept timeout — engine did not connect\n";
        return false;
    }
    setSocketBlocking(client_fd, true);
    sock_fd_ = static_cast<int>(client_fd);

    // Initialise stub state
    dummy_data_     = std::make_unique<SequenceProcessContext>();
    dummy_timeline_ = std::make_unique<NullTimelineFacade>();

    // Start reader thread BEFORE sending any commands
    alive_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    reader_thread_ = std::thread([this]{ readerLoop(); });

    // Send initial configuration
    {
        std::vector<uint8_t> p;
        writeU32(p, 2);  // default 2 input channels
        writeU32(p, 2);  // default 2 output channels
        sendFire(static_cast<uint32_t>(IpcMsgType::SetDefaultChannels), p);
    }
    {
        std::vector<uint8_t> p;
        writeBool(p, false);  // offline rendering off
        sendFire(static_cast<uint32_t>(IpcMsgType::OfflineRenderingSet), p);
    }
    {
        std::vector<uint8_t> p;
        writeBool(p, true);   // engine active
        sendFire(static_cast<uint32_t>(IpcMsgType::SetEngineActive), p);
    }

    return true;
}

// ── Destructor / shutdown ─────────────────────────────────────────────────

RemoteEngineProxy::~RemoteEngineProxy() {
    shutdown();
}

void RemoteEngineProxy::shutdown() {
    if (!running_.exchange(false))
        return;

    alive_.store(false, std::memory_order_release);

    // Tell engine to exit
    if (sock_fd_ >= 0) {
        std::lock_guard<std::mutex> lk(write_mutex_);
        std::vector<uint8_t> empty;
        ipcSendMsg(sock_fd_, IpcMsgType::Shutdown, 0, empty);
        CLOSE_SOCK(sock_fd_);
        sock_fd_ = -1;
    }

    if (reader_thread_.joinable())
        reader_thread_.join();

    // Wake any threads waiting on pending promises
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        for (auto& [id, req] : pending_) {
            if (req.p_i32) req.p_i32->set_value(-1);
            if (req.p_bool) req.p_bool->set_value(false);
            if (req.p_u8) req.p_u8->set_value(0xFF);
        }
        pending_.clear();
    }

#ifdef _WIN32
    if (child_proc_) {
        ::TerminateProcess(child_proc_, 0);
        ::WaitForSingleObject(child_proc_, 3000);
        ::CloseHandle(child_proc_);
        ::CloseHandle(child_thread_);
        child_proc_ = child_thread_ = nullptr;
    }
#else
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGTERM);
        ::waitpid(child_pid_, nullptr, 0);
        child_pid_ = -1;
    }
#endif

    // Unlink shared memory (creator's responsibility)
#ifndef _WIN32
    if (shm_)
        ::shm_unlink(shm_->name().c_str());
#endif
    shm_.reset();
}

// ── Reader thread ─────────────────────────────────────────────────────────

void RemoteEngineProxy::readerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        IpcHeader hdr{};
        std::vector<uint8_t> payload;
        if (!ipcRecvMsg(sock_fd_, hdr, payload)) {
            alive_.store(false, std::memory_order_release);
            break;
        }

        IpcReader r{payload.data(), payload.size()};
        const auto type = static_cast<IpcMsgType>(hdr.type);

        switch (type) {
        case IpcMsgType::AddEmptyTrackResult: {
            int32_t idx = r.i32();
            std::lock_guard<std::mutex> lk(pending_mutex_);
            if (auto it = pending_.find(hdr.req_id); it != pending_.end()) {
                if (it->second.p_i32) it->second.p_i32->set_value(idx);
                pending_.erase(it);
            }
            break;
        }
        case IpcMsgType::BoolResult: {
            bool val = r.bln();
            std::lock_guard<std::mutex> lk(pending_mutex_);
            if (auto it = pending_.find(hdr.req_id); it != pending_.end()) {
                if (it->second.p_bool) it->second.p_bool->set_value(val);
                if (it->second.p_i32)  it->second.p_i32->set_value(val ? 1 : 0);
                pending_.erase(it);
            }
            break;
        }
        case IpcMsgType::GetInstanceGroupResult: {
            uint8_t grp = r.u8();
            std::lock_guard<std::mutex> lk(pending_mutex_);
            if (auto it = pending_.find(hdr.req_id); it != pending_.end()) {
                if (it->second.p_u8) it->second.p_u8->set_value(grp);
                pending_.erase(it);
            }
            break;
        }
        case IpcMsgType::AddPluginCallback: {
            uint64_t cbId     = r.u64();
            int32_t  instId   = r.i32();
            int32_t  trackIdx = r.i32();
            std::string error = r.str();
            std::function<void(int32_t, int32_t, std::string)> cb;
            {
                std::lock_guard<std::mutex> lk(callbacks_mutex_);
                if (auto it = callbacks_.find(cbId); it != callbacks_.end()) {
                    cb = std::move(it->second);
                    callbacks_.erase(it);
                }
            }
            if (cb) cb(instId, trackIdx, error);
            break;
        }
        case IpcMsgType::PlaybackPositionResult: {
            int64_t pos = r.i64();
            playback_pos_.store(pos, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(pending_mutex_);
            if (auto it = pending_.find(hdr.req_id); it != pending_.end()) {
                if (it->second.p_i32) it->second.p_i32->set_value(static_cast<int32_t>(pos));
                pending_.erase(it);
            }
            break;
        }
        default:
            break;
        }
    }

    // Drain any still-pending requests
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        for (auto& [id, req] : pending_) {
            if (req.p_i32)  req.p_i32->set_value(-1);
            if (req.p_bool) req.p_bool->set_value(false);
            if (req.p_u8)   req.p_u8->set_value(0xFF);
        }
        pending_.clear();
    }
}

// ── Internal send helpers ─────────────────────────────────────────────────

bool RemoteEngineProxy::sendFire(uint32_t type, const std::vector<uint8_t>& payload) {
    if (!alive_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(write_mutex_);
    return ipcSendMsg(sock_fd_, static_cast<IpcMsgType>(type), 0, payload);
}

int32_t RemoteEngineProxy::callSyncI32(uint32_t type, uint64_t reqId,
    const std::vector<uint8_t>& payload,
    std::shared_ptr<std::promise<int32_t>> promise)
{
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_[reqId] = PendingRequest{promise, nullptr, nullptr};
    }
    if (!alive_.load(std::memory_order_acquire) ||
        ![&]{
            std::lock_guard<std::mutex> lk(write_mutex_);
            return ipcSendMsg(sock_fd_, static_cast<IpcMsgType>(type), reqId, payload);
        }()) {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_.erase(reqId);
        return -1;
    }
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_.erase(reqId);
        return -1;
    }
    return future.get();
}

bool RemoteEngineProxy::callSyncBool(uint32_t type, uint64_t reqId,
    const std::vector<uint8_t>& payload,
    std::shared_ptr<std::promise<bool>> promise)
{
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_[reqId] = PendingRequest{nullptr, promise, nullptr};
    }
    if (!alive_.load(std::memory_order_acquire) ||
        ![&]{
            std::lock_guard<std::mutex> lk(write_mutex_);
            return ipcSendMsg(sock_fd_, static_cast<IpcMsgType>(type), reqId, payload);
        }()) {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_.erase(reqId);
        return false;
    }
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_.erase(reqId);
        return false;
    }
    return future.get();
}

uint8_t RemoteEngineProxy::callSyncU8(uint32_t type, uint64_t reqId,
    const std::vector<uint8_t>& payload,
    std::shared_ptr<std::promise<uint8_t>> promise)
{
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_[reqId] = PendingRequest{nullptr, nullptr, promise};
    }
    if (!alive_.load(std::memory_order_acquire) ||
        ![&]{
            std::lock_guard<std::mutex> lk(write_mutex_);
            return ipcSendMsg(sock_fd_, static_cast<IpcMsgType>(type), reqId, payload);
        }()) {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_.erase(reqId);
        return 0xFF;
    }
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_.erase(reqId);
        return 0xFF;
    }
    return future.get();
}

// ── Audio round-trip (audio thread — lock-free path) ──────────────────────

uapmd_status_t RemoteEngineProxy::processAudio(AudioProcessContext& process) {
    if (!alive_.load(std::memory_order_acquire)) {
        process.clearAudioOutputs();
        return 0;  // silence, don't crash host
    }

    auto& mc  = process.masterContext();
    auto& reg = *region_;

    // ── Fill shared region with this callback's inputs ──────────────────
    const uint32_t nFrames = static_cast<uint32_t>(process.frameCount());
    const uint32_t nIn     = static_cast<uint32_t>(process.inputChannelCount(0));
    const uint32_t nOut    = static_cast<uint32_t>(process.outputChannelCount(0));

    reg.frame_count               = nFrames;
    reg.in_channels               = nIn;
    reg.out_channels              = nOut;
    reg.sample_rate               = mc.sampleRate();
    reg.is_playing                = mc.isPlaying();
    reg.playback_position_samples = mc.playbackPositionSamples();
    reg.tempo                     = mc.tempo();
    reg.time_sig_num              = mc.timeSignatureNumerator();
    reg.time_sig_den              = mc.timeSignatureDenominator();

    // Audio input channels
    const uint32_t copyIn = std::min(nIn, kSharedAudioMaxChannels);
    for (uint32_t ch = 0; ch < copyIn; ++ch) {
        const float* src = process.getFloatInBuffer(0, ch);
        if (src)
            std::memcpy(reg.audio_in[ch], src, nFrames * sizeof(float));
        else
            std::memset(reg.audio_in[ch], 0, nFrames * sizeof(float));
    }

    // Event input
    {
        auto& evIn = process.eventIn();
        uint32_t evSize = std::min(static_cast<size_t>(kSharedEventMaxBytes), evIn.position());
        reg.event_in_size = evSize;
        if (evSize > 0)
            std::memcpy(reg.event_in_data, evIn.getMessages(), evSize);
    }

    // ── Signal engine ────────────────────────────────────────────────────
    const uint64_t expected = ++expected_engine_seq_;
    reg.host_seq.fetch_add(1, std::memory_order_release);

    // ── Spin-wait for engine response ────────────────────────────────────
    // Check every ~1024 iterations whether we've timed out (avoids
    // calling steady_clock::now() on every iteration).
    constexpr int kCheckInterval = 1024;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (int spin = 0;;) {
        if (reg.engine_seq.load(std::memory_order_acquire) >= expected)
            break;
        if (++spin % kCheckInterval == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                alive_.store(false, std::memory_order_release);
                process.clearAudioOutputs();
                return 0;
            }
        }
    }

    // ── Copy output back to AudioProcessContext ──────────────────────────
    const uint32_t copyOut = std::min(nOut, kSharedAudioMaxChannels);
    for (uint32_t ch = 0; ch < copyOut; ++ch) {
        float* dst = process.getFloatOutBuffer(0, ch);
        if (dst)
            std::memcpy(dst, reg.audio_out[ch], nFrames * sizeof(float));
    }

    // Update cached playback position from region (engine may advance it)
    playback_pos_.store(reg.playback_position_samples, std::memory_order_relaxed);

    return 0;
}

// ── Transport (fire-and-forget) ───────────────────────────────────────────

// The real engine only starts/stops playback via these explicit commands
// (processAudio never auto-stops), so mirroring the state locally is exact.
void RemoteEngineProxy::startPlayback() {
    playback_active_.store(true, std::memory_order_relaxed);
    sendFire(static_cast<uint32_t>(IpcMsgType::StartPlayback), {});
}
void RemoteEngineProxy::stopPlayback() {
    playback_active_.store(false, std::memory_order_relaxed);
    sendFire(static_cast<uint32_t>(IpcMsgType::StopPlayback), {});
}
void RemoteEngineProxy::pausePlayback() {
    playback_active_.store(false, std::memory_order_relaxed);
    sendFire(static_cast<uint32_t>(IpcMsgType::PausePlayback), {});
}
void RemoteEngineProxy::resumePlayback() {
    playback_active_.store(true, std::memory_order_relaxed);
    sendFire(static_cast<uint32_t>(IpcMsgType::ResumePlayback), {});
}

void RemoteEngineProxy::playbackPosition(int64_t samples) {
    std::vector<uint8_t> p;
    writeI64(p, samples);
    sendFire(static_cast<uint32_t>(IpcMsgType::Seek), p);
}

bool RemoteEngineProxy::isPlaybackActive() const {
    return playback_active_.load(std::memory_order_relaxed);
}

int64_t RemoteEngineProxy::playbackPosition() const {
    return playback_pos_.load(std::memory_order_relaxed);
}

// ── Configuration ─────────────────────────────────────────────────────────

void RemoteEngineProxy::setDefaultChannels(uint32_t in, uint32_t out) {
    std::vector<uint8_t> p;
    writeU32(p, in);
    writeU32(p, out);
    sendFire(static_cast<uint32_t>(IpcMsgType::SetDefaultChannels), p);
}

void RemoteEngineProxy::setEngineActive(bool active) {
    std::vector<uint8_t> p;
    writeBool(p, active);
    sendFire(static_cast<uint32_t>(IpcMsgType::SetEngineActive), p);
}

bool RemoteEngineProxy::offlineRendering() const {
    return offline_rendering_.load(std::memory_order_relaxed);
}

void RemoteEngineProxy::offlineRendering(bool enabled) {
    offline_rendering_.store(enabled, std::memory_order_relaxed);
    std::vector<uint8_t> p;
    writeBool(p, enabled);
    sendFire(static_cast<uint32_t>(IpcMsgType::OfflineRenderingSet), p);
}

void RemoteEngineProxy::cleanupEmptyTracks() {
    sendFire(static_cast<uint32_t>(IpcMsgType::CleanupEmptyTracks), {});
}

// ── Structural mutations (synchronous) ────────────────────────────────────

uapmd_track_index_t RemoteEngineProxy::addEmptyTrack() {
    uint64_t reqId = nextReqId();
    auto promise = std::make_shared<std::promise<int32_t>>();
    return callSyncI32(static_cast<uint32_t>(IpcMsgType::AddEmptyTrack),
                       reqId, {}, std::move(promise));
}

void RemoteEngineProxy::addPluginToTrack(uapmd_track_index_t trackIndex,
    std::string& format, std::string& pluginId,
    std::function<void(int32_t, uapmd_track_index_t, std::string)> callback)
{
    const uint64_t cbId = nextReqId();
    {
        std::lock_guard<std::mutex> lk(callbacks_mutex_);
        callbacks_[cbId] = std::move(callback);
    }
    std::vector<uint8_t> p;
    writeI32(p, trackIndex);
    writeStr(p, format);
    writeStr(p, pluginId);
    writeU64(p, cbId);
    sendFire(static_cast<uint32_t>(IpcMsgType::AddPlugin), p);
}

bool RemoteEngineProxy::removePluginInstance(int32_t instanceId) {
    uint64_t reqId = nextReqId();
    auto promise = std::make_shared<std::promise<bool>>();
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    return callSyncBool(static_cast<uint32_t>(IpcMsgType::RemovePlugin),
                        reqId, p, std::move(promise));
}

bool RemoteEngineProxy::removeTrack(uapmd_track_index_t trackIndex) {
    uint64_t reqId = nextReqId();
    auto promise = std::make_shared<std::promise<bool>>();
    std::vector<uint8_t> p;
    writeI32(p, trackIndex);
    return callSyncBool(static_cast<uint32_t>(IpcMsgType::RemoveTrack),
                        reqId, p, std::move(promise));
}

uint8_t RemoteEngineProxy::getInstanceGroup(int32_t instanceId) const {
    uint64_t reqId = const_cast<RemoteEngineProxy*>(this)->nextReqId();
    auto promise = std::make_shared<std::promise<uint8_t>>();
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    return const_cast<RemoteEngineProxy*>(this)->callSyncU8(
        static_cast<uint32_t>(IpcMsgType::GetInstanceGroup), reqId, p, std::move(promise));
}

bool RemoteEngineProxy::setInstanceGroup(int32_t instanceId, uint8_t group) {
    uint64_t reqId = nextReqId();
    auto promise = std::make_shared<std::promise<bool>>();
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeU8(p, group);
    return callSyncBool(static_cast<uint32_t>(IpcMsgType::SetInstanceGroup),
                        reqId, p, std::move(promise));
}

// ── MIDI / UMP events (fire-and-forget) ──────────────────────────────────

void RemoteEngineProxy::enqueueUmp(int32_t instanceId, uapmd_ump_t* ump,
                                    size_t sizeInBytes, uapmd_timestamp_t timestamp) {
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeU64(p, static_cast<uint64_t>(timestamp));
    writeU32(p, static_cast<uint32_t>(sizeInBytes));
    writeBytes(p, ump, sizeInBytes);
    sendFire(static_cast<uint32_t>(IpcMsgType::EnqueueUmp), p);
}

void RemoteEngineProxy::sendNoteOn(int32_t instanceId, int32_t note) {
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeI32(p, note);
    sendFire(static_cast<uint32_t>(IpcMsgType::SendNoteOn), p);
}

void RemoteEngineProxy::sendNoteOff(int32_t instanceId, int32_t note) {
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeI32(p, note);
    sendFire(static_cast<uint32_t>(IpcMsgType::SendNoteOff), p);
}

void RemoteEngineProxy::sendPitchBend(int32_t instanceId, float normalizedValue) {
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeF32(p, normalizedValue);
    sendFire(static_cast<uint32_t>(IpcMsgType::SendPitchBend), p);
}

void RemoteEngineProxy::sendChannelPressure(int32_t instanceId, float pressure) {
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeF32(p, pressure);
    sendFire(static_cast<uint32_t>(IpcMsgType::SendChannelPressure), p);
}

void RemoteEngineProxy::setParameterValue(int32_t instanceId, int32_t index, double value) {
    std::vector<uint8_t> p;
    writeI32(p, instanceId);
    writeI32(p, index);
    writeF64(p, value);
    sendFire(static_cast<uint32_t>(IpcMsgType::SetParameterValue), p);
}

// ── Spectrum analysis (Phase 2 stub: returns silence) ────────────────────

void RemoteEngineProxy::getInputSpectrum(float* outSpectrum, int numBars) const {
    if (outSpectrum && numBars > 0)
        std::memset(outSpectrum, 0, static_cast<size_t>(numBars) * sizeof(float));
}

void RemoteEngineProxy::getOutputSpectrum(float* outSpectrum, int numBars) const {
    if (outSpectrum && numBars > 0)
        std::memset(outSpectrum, 0, static_cast<size_t>(numBars) * sizeof(float));
}

// ── Stub accessors ────────────────────────────────────────────────────────

std::vector<SequencerTrack*>& RemoteEngineProxy::tracks() const {
    return dummy_tracks_;
}

SequenceProcessContext& RemoteEngineProxy::data() {
    return *dummy_data_;
}

TimelineFacade& RemoteEngineProxy::timeline() {
    return *dummy_timeline_;
}

} // namespace uapmd::ipc
