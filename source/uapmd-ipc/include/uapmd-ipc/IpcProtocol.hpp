#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace uapmd::ipc {

// ── Wire message types ────────────────────────────────────────────────────
// All messages are framed as:  [IpcHeader (16 bytes)] [payload (header.size bytes)]
// Fields are little-endian throughout.

enum class IpcMsgType : uint32_t {
    // ── Host → Engine (commands) ──────────────────────────────────────────
    SetDefaultChannels  =  1,   // u32 inCh, u32 outCh
    SetEngineActive     =  2,   // bool active
    OfflineRenderingSet =  3,   // bool enabled
    AddEmptyTrack       =  4,   // (no payload)  → AddEmptyTrackResult
    AddPlugin           =  5,   // i32 trackIdx, str format, str pluginId, u64 cbId
    RemovePlugin        =  6,   // i32 instanceId  → BoolResult
    RemoveTrack         =  7,   // i32 trackIdx    → BoolResult
    StartPlayback       =  8,
    StopPlayback        =  9,
    PausePlayback       = 10,
    ResumePlayback      = 11,
    Seek                = 12,   // i64 samples
    CleanupEmptyTracks  = 13,
    EnqueueUmp          = 14,   // i32 instanceId, u64 timestamp, u32 size, bytes...
    SendNoteOn          = 15,   // i32 instanceId, i32 note
    SendNoteOff         = 16,   // i32 instanceId, i32 note
    SendPitchBend       = 17,   // i32 instanceId, f32 value
    SendChannelPressure = 18,   // i32 instanceId, f32 pressure
    SetParameterValue   = 19,   // i32 instanceId, i32 index, f64 value
    GetInstanceGroup    = 20,   // i32 instanceId  → GetInstanceGroupResult
    SetInstanceGroup    = 21,   // i32 instanceId, u8 group  → BoolResult
    OfflineRenderingGet = 22,   // (no payload)    → BoolResult
    PlaybackActiveGet   = 23,   // (no payload)    → BoolResult
    PlaybackPositionGet = 24,   // (no payload)    → PlaybackPositionResult
    Shutdown            = 25,

    // ── Engine → Host (responses) ─────────────────────────────────────────
    AddEmptyTrackResult    = 100,  // i32 trackIdx
    BoolResult             = 101,  // bool result
    AddPluginCallback      = 102,  // u64 cbId, i32 instanceId, i32 trackIdx, str error
    GetInstanceGroupResult = 103,  // u8 group
    PlaybackPositionResult = 104,  // i64 samples
};

// Fixed-size message header (16 bytes, always little-endian)
struct IpcHeader {
    uint32_t type;    // IpcMsgType cast to uint32_t
    uint32_t size;    // payload size in bytes (NOT counting this header)
    uint64_t req_id;  // non-zero for request/response pairs; 0 for fire-and-forget
};
static_assert(sizeof(IpcHeader) == 16, "IpcHeader must be 16 bytes");

// ── Payload serialization helpers ─────────────────────────────────────────

inline void writeU8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

inline void writeU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

inline void writeU64(std::vector<uint8_t>& buf, uint64_t v) {
    writeU32(buf, static_cast<uint32_t>(v));
    writeU32(buf, static_cast<uint32_t>(v >> 32));
}

inline void writeI32(std::vector<uint8_t>& buf, int32_t v)  { writeU32(buf, static_cast<uint32_t>(v)); }
inline void writeI64(std::vector<uint8_t>& buf, int64_t v)  { writeU64(buf, static_cast<uint64_t>(v)); }
inline void writeBool(std::vector<uint8_t>& buf, bool v)    { buf.push_back(v ? 1 : 0); }
inline void writeF32(std::vector<uint8_t>& buf, float v) {
    uint32_t bits; std::memcpy(&bits, &v, 4); writeU32(buf, bits);
}
inline void writeF64(std::vector<uint8_t>& buf, double v) {
    uint64_t bits; std::memcpy(&bits, &v, 8); writeU64(buf, bits);
}
inline void writeStr(std::vector<uint8_t>& buf, const std::string& s) {
    writeU32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}
inline void writeBytes(std::vector<uint8_t>& buf, const void* src, size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), p, p + n);
}

// ── Payload deserialization helper ────────────────────────────────────────

struct IpcReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos{0};

    bool ok() const { return pos <= size; }

    uint8_t u8() {
        uint8_t v = data[pos]; pos += 1; return v;
    }
    uint32_t u32() {
        uint32_t v = static_cast<uint32_t>(data[pos])
                   | (static_cast<uint32_t>(data[pos+1]) << 8)
                   | (static_cast<uint32_t>(data[pos+2]) << 16)
                   | (static_cast<uint32_t>(data[pos+3]) << 24);
        pos += 4; return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    int32_t  i32()  { return static_cast<int32_t>(u32()); }
    int64_t  i64()  { return static_cast<int64_t>(u64()); }
    bool     bln()  { return u8() != 0; }
    float    f32()  { uint32_t bits = u32(); float  v; std::memcpy(&v, &bits, 4); return v; }
    double   f64()  { uint64_t bits = u64(); double v; std::memcpy(&v, &bits, 8); return v; }
    std::string str() {
        uint32_t len = u32();
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len; return s;
    }
    const uint8_t* bytes(size_t n) {
        const uint8_t* p = data + pos;
        pos += n;
        return p;
    }
};

// ── Socket I/O helpers (platform-transparent) ─────────────────────────────
// Declared here, defined in a platform .cpp so callers don't need OS headers.

// Read exactly `n` bytes from `fd`; returns false on error/EOF.
bool ipcReadExact(int fd, void* buf, size_t n);

// Write exactly `n` bytes to `fd`; returns false on error.
bool ipcWriteExact(int fd, const void* buf, size_t n);

// Read a full framed message (header + payload) from `fd`.
// Returns false on error.  Caller owns the payload vector.
bool ipcRecvMsg(int fd, IpcHeader& hdr, std::vector<uint8_t>& payload);

// Write a framed message (header + payload) to `fd`.
// Thread-safe: does NOT lock — caller must hold a write mutex if needed.
bool ipcSendMsg(int fd, IpcMsgType type, uint64_t reqId, const std::vector<uint8_t>& payload);

} // namespace uapmd::ipc
