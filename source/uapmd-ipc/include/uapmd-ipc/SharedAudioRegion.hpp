#pragma once

#include <atomic>
#include <cstdint>

namespace uapmd::ipc {

// Maximum channels and frames supported in the shared audio buffer.
// Increase if larger configs are needed; the struct size grows accordingly.
static constexpr uint32_t kSharedAudioMaxChannels = 8;
static constexpr uint32_t kSharedAudioMaxFrames   = 4096;
static constexpr uint32_t kSharedEventMaxBytes    = 65536;

// Flat POD struct placed at the start of a shared memory segment.
// Both host and engine processes map the SAME physical pages, so:
//   • No pointers, no vtables, no non-trivial ctors/dtors.
//   • std::atomic<uint64_t> must be lock-free (static_assert below).
//
// Audio round-trip protocol (per processAudio() call):
//   Host: fill audio_in / event_in / metadata, then
//         host_seq.fetch_add(1, release)          [signals "input ready"]
//   Engine: spin until host_seq > last_seen_seq (acquire),
//           fill audio_out / event_out, then
//           engine_seq.store(host_seq, release)   [signals "output ready"]
//   Host: spin until engine_seq >= expected (acquire), read outputs.
struct SharedAudioRegion {
    // ── Sync counters (each on its own cache line to avoid false sharing) ───
    alignas(64) std::atomic<uint64_t> host_seq{0};
    alignas(64) std::atomic<uint64_t> engine_seq{0};

    // ── Per-callback metadata (written by host BEFORE releasing host_seq) ──
    uint32_t frame_count{0};
    uint32_t in_channels{0};
    uint32_t out_channels{0};
    int32_t  sample_rate{48000};
    bool     is_playing{false};
    uint8_t  _pad0[3];
    int64_t  playback_position_samples{0};
    uint32_t tempo{500000};            // microseconds per quarter note
    int32_t  time_sig_num{4};
    int32_t  time_sig_den{4};

    // ── Event/UMP data ──────────────────────────────────────────────────────
    uint32_t event_in_size{0};
    uint8_t  event_in_data[kSharedEventMaxBytes];

    uint32_t event_out_size{0};
    uint8_t  event_out_data[kSharedEventMaxBytes];

    // ── Audio sample buffers (interleaved per channel, float32) ────────────
    float audio_in [kSharedAudioMaxChannels][kSharedAudioMaxFrames];
    float audio_out[kSharedAudioMaxChannels][kSharedAudioMaxFrames];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "SharedAudioRegion requires lock-free 64-bit atomics");

} // namespace uapmd::ipc
