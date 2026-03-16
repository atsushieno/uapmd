#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd::ipc {

// ─── Structural mutations ─────────────────────────────────────────────────
// These modify the track/plugin graph. In ThreadedEngineProxy they are
// forwarded directly; in RemoteEngineProxy they will be serialised over the
// control socket. The optional promise fields carry synchronous return values.

struct AddTrackCmd {
    std::shared_ptr<std::promise<uapmd_track_index_t>> result;
};

struct RemoveTrackCmd {
    uapmd_track_index_t trackIndex;
    std::shared_ptr<std::promise<bool>> result;
};

struct AddPluginCmd {
    uapmd_track_index_t trackIndex;
    std::string format;
    std::string pluginId;
    std::function<void(int32_t instanceId, uapmd_track_index_t trackIndex, std::string error)> callback;
};

struct RemovePluginCmd {
    int32_t instanceId;
    std::shared_ptr<std::promise<bool>> result;
};

struct SetInstanceGroupCmd {
    int32_t instanceId;
    uint8_t group;
    std::shared_ptr<std::promise<bool>> result;
};

// ─── Transport commands ──────────────────────────────────────────────────
// These are queued so they take effect at the start of the next audio
// callback (clean buffer boundary) rather than mid-buffer.

struct StartPlaybackCmd {};
struct StopPlaybackCmd {};
struct PausePlaybackCmd {};
struct ResumePlaybackCmd {};

struct SeekCmd {
    int64_t samples;
};

// ─── Configuration ───────────────────────────────────────────────────────

struct SetDefaultChannelsCmd {
    uint32_t inputChannels;
    uint32_t outputChannels;
};

struct CleanupEmptyTracksCmd {};

// ─── Wire variant ─────────────────────────────────────────────────────────
// All command types in one discriminated union. Add new commands here as
// the protocol grows. RemoteEngineProxy will serialise the active index and
// fields over the control socket.

using EngineCommand = std::variant<
    AddTrackCmd,
    RemoveTrackCmd,
    AddPluginCmd,
    RemovePluginCmd,
    SetInstanceGroupCmd,
    StartPlaybackCmd,
    StopPlaybackCmd,
    PausePlaybackCmd,
    ResumePlaybackCmd,
    SeekCmd,
    SetDefaultChannelsCmd,
    CleanupEmptyTracksCmd
>;

} // namespace uapmd::ipc
