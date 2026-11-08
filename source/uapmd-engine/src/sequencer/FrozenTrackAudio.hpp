#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd {

// A file read and written at explicit offsets, held exclusively by this process while open.
class FrozenTrackAudioFile {
public:
    static std::unique_ptr<FrozenTrackAudioFile> create(const std::filesystem::path& path, std::string& error);
    // Fails when another process holds the file.
    static std::unique_ptr<FrozenTrackAudioFile> open(const std::filesystem::path& path, std::string& error);
    // Removes the file unless another process holds it.
    static bool removeUnlessHeld(const std::filesystem::path& path);
    ~FrozenTrackAudioFile();
    FrozenTrackAudioFile(const FrozenTrackAudioFile&) = delete;
    FrozenTrackAudioFile& operator=(const FrozenTrackAudioFile&) = delete;

    bool writeAt(uint64_t offset, const void* data, size_t size);
    bool readAt(uint64_t offset, void* data, size_t size) const;
    bool size(uint64_t& size) const;
    bool rename(const std::filesystem::path& path);
    void removeOnClose(bool remove) { remove_on_close_ = remove; }
    const std::filesystem::path& path() const { return path_; }

private:
    FrozenTrackAudioFile() = default;

#if defined(_WIN32)
    void* handle_{nullptr};
#else
    int fd_{-1};
#endif
    std::filesystem::path path_;
    bool remove_on_close_{false};
};

// What a stored render must still match when a later session reuses it.
struct FrozenTrackAudioIdentity {
    int32_t sampleRate{0};
    // Realtime graph latency per output bus.
    std::vector<uint32_t> busLatencies;
    // Format and plugin ID per plugin, in graph order.
    std::vector<std::string> plugins;
    bool operator==(const FrozenTrackAudioIdentity&) const = default;
};

// Frozen track audio kept on disk; only a short window around the playhead is held in memory.
// Each chunk is stored as runs of silence and literal samples, expanded by the loader; a silent chunk takes no disk space.
// The render thread writes it through the sink, then the main thread finishes it for playback.
// During playback the audio thread reads the window and a loader thread fills it ahead.
// A file stays a temporary one, removed when released, until a saved document refers to it.
class FrozenTrackAudio final : public OfflineTrackRenderSink {
public:
    static std::shared_ptr<FrozenTrackAudio> create(const std::filesystem::path& directory, std::string& error);
    // A stored render that persist() kept for a saved document.
    static std::shared_ptr<FrozenTrackAudio> open(
        const std::filesystem::path& directory, const std::string& token, std::string& error);
    // Removes temporary files no process holds.
    static void removeAbandonedFiles(const std::filesystem::path& directory);
    // Also removes the least recently used stored renders beyond limitBytes, except keptTokens, whatever their size.
    static void collectGarbage(
        const std::filesystem::path& directory, uint64_t limitBytes, const std::vector<std::string>& keptTokens);

    bool writeFrames(const float* const* channels, uint32_t channelCount, uint32_t frames, std::string& error) override;

    // Main thread, once rendering is complete; shifts are per bus, as the render result reports them.
    bool finishWriting(
        int64_t startSample,
        const std::vector<uint32_t>& busChannelCounts,
        const std::vector<int64_t>& busLatencyShifts,
        FrozenTrackAudioIdentity identity,
        std::string& error);
    // Main thread. Keeps the file after release, for a saved document that refers to token().
    bool persist();

    const std::string& token() const { return token_; }
    const FrozenTrackAudioIdentity& identity() const { return identity_; }
    int64_t startSample() const { return start_sample_; }
    uint32_t channelCount() const { return channel_count_; }

    // Audio thread. Writes frames of channel at output frame position; data not loaded yet plays as
    // silence, unless waitForDisk, which offline rendering uses as it may block.
    void read(uint32_t channel, int64_t position, float* output, int32_t frames, bool waitForDisk) noexcept;
    // Audio thread. Tells the loader where playback is.
    void playbackPosition(int64_t position) noexcept { play_position_.store(position, std::memory_order_release); }

    // Loader thread. Loads what playback needs next; returns false when nothing was missing.
    bool loadAhead();

    struct Summary {
        uint64_t frames{0};
        float peak{0.0f};
        double rms{0.0};
    };
    // Reads the whole file, as it plays; any thread but the audio thread.
    Summary summarize() const;

private:
    struct Slot {
        std::atomic<int64_t> chunk{-1};
        std::vector<float> samples;
    };

    struct StoredChunk {
        uint64_t offset{0};
        // 0 for a silent chunk.
        uint32_t size{0};
    };

    FrozenTrackAudio() = default;
    bool flushChunk(std::string& error);
    bool writeTrailer(std::string& error);
    bool readTrailer(std::string& error);
    bool setChannelShifts(const std::vector<uint32_t>& busChannelCounts, const std::vector<int64_t>& busLatencyShifts, std::string& error);
    void preparePlayback();
    float* chunkSamples(uint32_t slot, uint32_t channel) { return slots_[slot].samples.data() + static_cast<size_t>(channel) * kChunkFrames; }
    // Expands a stored chunk into samples; encoded must have capacity for the largest one, so that it never allocates.
    bool loadChunk(int64_t chunk, float* samples, std::vector<uint8_t>& encoded) const noexcept;
    void neededChunks(int64_t position, int64_t& first, int64_t& last) const;

    static constexpr uint32_t kChunkFrames = 8192;

    std::unique_ptr<FrozenTrackAudioFile> file_;
    std::string token_;
    std::filesystem::path directory_;
    bool persisted_{false};
    FrozenTrackAudioIdentity identity_;
    uint32_t channel_count_{0};
    int64_t start_sample_{0};
    int64_t rendered_frames_{0};
    int64_t chunk_count_{0};

    std::vector<StoredChunk> stored_chunks_;
    std::vector<uint32_t> bus_channel_counts_;
    std::vector<int64_t> bus_latency_shifts_;
    uint32_t largest_stored_chunk_{0};

    // Rendering
    std::vector<float> staging_;
    std::vector<uint8_t> encoding_;
    uint32_t staging_frames_{0};
    uint64_t write_offset_{0};

    // Playback
    std::vector<int64_t> channel_shifts_;
    int64_t minimum_shift_{0};
    int64_t maximum_shift_{0};
    int64_t lookahead_chunks_{0};
    std::unique_ptr<Slot[]> slots_;
    uint32_t slot_count_{0};
    std::atomic<int64_t> play_position_{0};
    // The slot the audio thread is copying from; the loader never overwrites it.
    std::atomic<int32_t> reading_slot_{-1};
    std::vector<uint8_t> loader_encoded_;
    // Offline rendering reads the disk itself; the last chunk it expanded is kept.
    std::vector<uint8_t> blocking_encoded_;
    std::vector<float> blocking_samples_;
    int64_t blocking_chunk_{-1};
};

} // namespace uapmd
