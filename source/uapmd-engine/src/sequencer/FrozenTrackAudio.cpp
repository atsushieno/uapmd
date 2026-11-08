#include "FrozenTrackAudio.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <random>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uapmd {
namespace {

constexpr double kLookaheadSeconds = 1.0;
// Shorter runs of silence stay literal, so that tokens do not outweigh the samples they save.
constexpr uint32_t kMinimumSilentRun = 16;
constexpr char kTrailerMagic[8]{'U', 'A', 'P', 'M', 'D', 'F', 'Z', '1'};
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kFooterSize = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(kTrailerMagic);
constexpr uint32_t kMaximumTrailerSize = 64u * 1024u * 1024u;
constexpr std::string_view kTemporaryExtension{".tmp"};
constexpr std::string_view kStoredExtension{".frozen"};

bool isSilent(float sample) {
    return std::bit_cast<uint32_t>(sample) == 0;
}

uint32_t silentRunLength(const float* samples, uint32_t begin, uint32_t end) {
    auto index = begin;
    while (index < end && isSilent(samples[index]))
        ++index;
    return index - begin;
}

void appendBytes(std::vector<uint8_t>& output, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    output.insert(output.end(), bytes, bytes + size);
}

// Per channel: tokens of (silent frames, literal frames, literal samples) until the chunk is covered.
void encodeChunk(const float* samples, uint32_t channelCount, uint32_t chunkFrames, std::vector<uint8_t>& output) {
    output.clear();
    for (uint32_t channel = 0; channel < channelCount; ++channel) {
        const auto* data = samples + static_cast<size_t>(channel) * chunkFrames;
        uint32_t index = 0;
        while (index < chunkFrames) {
            auto silent = silentRunLength(data, index, chunkFrames);
            if (silent < kMinimumSilentRun && index + silent < chunkFrames)
                silent = 0;
            const auto literalBegin = index + silent;
            auto literalEnd = literalBegin;
            while (literalEnd < chunkFrames) {
                if (!isSilent(data[literalEnd])) {
                    ++literalEnd;
                    continue;
                }
                const auto run = silentRunLength(data, literalEnd, chunkFrames);
                if (run >= kMinimumSilentRun || literalEnd + run == chunkFrames)
                    break;
                literalEnd += run;
            }
            const uint32_t literals = literalEnd - literalBegin;
            appendBytes(output, &silent, sizeof(silent));
            appendBytes(output, &literals, sizeof(literals));
            appendBytes(output, data + literalBegin, literals * sizeof(float));
            index = literalEnd;
        }
    }
}

bool decodeChunk(const uint8_t* input, size_t size, float* samples, uint32_t channelCount, uint32_t chunkFrames) noexcept {
    size_t position = 0;
    for (uint32_t channel = 0; channel < channelCount; ++channel) {
        auto* data = samples + static_cast<size_t>(channel) * chunkFrames;
        uint32_t index = 0;
        while (index < chunkFrames) {
            uint32_t silent, literals;
            if (size - position < sizeof(silent) + sizeof(literals))
                return false;
            std::memcpy(&silent, input + position, sizeof(silent));
            std::memcpy(&literals, input + position + sizeof(silent), sizeof(literals));
            position += sizeof(silent) + sizeof(literals);
            if ((silent == 0 && literals == 0) || silent > chunkFrames - index ||
                literals > chunkFrames - index - silent ||
                size - position < static_cast<size_t>(literals) * sizeof(float))
                return false;
            std::fill_n(data + index, silent, 0.0f);
            index += silent;
            std::memcpy(data + index, input + position, static_cast<size_t>(literals) * sizeof(float));
            position += static_cast<size_t>(literals) * sizeof(float);
            index += literals;
        }
    }
    return position == size;
}

int64_t floorDivide(int64_t value, int64_t divisor) {
    const auto quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

class ByteWriter {
public:
    template <typename T>
    void value(T value) { appendBytes(bytes, &value, sizeof(value)); }
    void string(const std::string& value) {
        this->value(static_cast<uint32_t>(value.size()));
        appendBytes(bytes, value.data(), value.size());
    }
    std::vector<uint8_t> bytes;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    template <typename T>
    T value() {
        T result{};
        if (size_ - position_ < sizeof(T)) {
            ok_ = false;
            return result;
        }
        std::memcpy(&result, data_ + position_, sizeof(T));
        position_ += sizeof(T);
        return result;
    }
    std::string string() {
        const auto length = value<uint32_t>();
        if (!ok_ || size_ - position_ < length) {
            ok_ = false;
            return {};
        }
        std::string result(reinterpret_cast<const char*>(data_ + position_), length);
        position_ += length;
        return result;
    }
    // Rejects counts that could not fit in what is left, before anything is allocated for them.
    bool fits(uint64_t count, size_t itemSize) {
        ok_ = ok_ && count <= (size_ - position_) / itemSize;
        return ok_;
    }
    bool ok() const { return ok_; }
    bool atEnd() const { return position_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_{0};
    bool ok_{true};
};

std::string newToken() {
    std::random_device device;
    std::mt19937_64 generator(
        (static_cast<uint64_t>(device()) << 32) ^ device() ^
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return std::format("{:016x}{:016x}", generator(), generator());
}

bool isToken(const std::string& token) {
    return token.size() == 32 &&
        std::ranges::all_of(token, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

#if !defined(_WIN32)
bool lockExclusively(int fd) {
#if defined(__EMSCRIPTEN__)
    (void) fd;
    return true;
#else
    while (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        if (errno != EINTR)
            return false;
    return true;
#endif
}
#endif

} // namespace

std::unique_ptr<FrozenTrackAudioFile> FrozenTrackAudioFile::create(
    const std::filesystem::path& path,
    std::string& error) {
    std::unique_ptr<FrozenTrackAudioFile> file(new FrozenTrackAudioFile());
#if defined(_WIN32)
    auto handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::format("Cannot create the frozen track file {}.", path.string());
        return nullptr;
    }
    file->handle_ = handle;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = std::format("Cannot create the frozen track file {}: {}", path.string(), std::strerror(errno));
        return nullptr;
    }
    file->fd_ = fd;
    if (!lockExclusively(fd)) {
        ::unlink(path.c_str());
        error = std::format("Cannot lock the frozen track file {}.", path.string());
        return nullptr;
    }
#endif
    file->path_ = path;
    return file;
}

std::unique_ptr<FrozenTrackAudioFile> FrozenTrackAudioFile::open(
    const std::filesystem::path& path,
    std::string& error) {
    std::unique_ptr<FrozenTrackAudioFile> file(new FrozenTrackAudioFile());
#if defined(_WIN32)
    auto handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::format("Cannot open the frozen track file {}.", path.string());
        return nullptr;
    }
    file->handle_ = handle;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = std::format("Cannot open the frozen track file {}: {}", path.string(), std::strerror(errno));
        return nullptr;
    }
    file->fd_ = fd;
    if (!lockExclusively(fd)) {
        error = std::format("The frozen track file {} is in use.", path.string());
        return nullptr;
    }
#endif
    file->path_ = path;
    return file;
}

bool FrozenTrackAudioFile::removeUnlessHeld(const std::filesystem::path& path) {
#if defined(_WIN32)
    // Every holder opens without delete sharing, so this fails while one does.
    return DeleteFileW(path.c_str()) != 0;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool removed = lockExclusively(fd) && ::unlink(path.c_str()) == 0;
    ::close(fd);
    return removed;
#endif
}

FrozenTrackAudioFile::~FrozenTrackAudioFile() {
#if defined(_WIN32)
    if (!handle_)
        return;
    if (remove_on_close_) {
        // Aggregate initialized: windows.h defines DeleteFile as a macro.
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(handle_);
#else
    if (fd_ < 0)
        return;
    // Removed while still locked, so that no other process adopts it in between.
    if (remove_on_close_)
        ::unlink(path_.c_str());
    ::close(fd_);
#endif
}

bool FrozenTrackAudioFile::writeAt(uint64_t offset, const void* data, size_t size) {
    auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0) {
#if defined(_WIN32)
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written = 0;
        const auto request = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
        if (!WriteFile(handle_, bytes, request, &written, &overlapped) || written == 0)
            return false;
#else
        const auto written = ::pwrite(fd_, bytes, size, static_cast<off_t>(offset));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
#endif
        bytes += written;
        offset += static_cast<uint64_t>(written);
        size -= static_cast<size_t>(written);
    }
    return true;
}

bool FrozenTrackAudioFile::readAt(uint64_t offset, void* data, size_t size) const {
    auto* bytes = static_cast<uint8_t*>(data);
    while (size > 0) {
#if defined(_WIN32)
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        const auto request = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
        if (!ReadFile(handle_, bytes, request, &read, &overlapped) || read == 0)
            return false;
#else
        const auto read = ::pread(fd_, bytes, size, static_cast<off_t>(offset));
        if (read < 0 && errno == EINTR)
            continue;
        if (read <= 0)
            return false;
#endif
        bytes += read;
        offset += static_cast<uint64_t>(read);
        size -= static_cast<size_t>(read);
    }
    return true;
}

bool FrozenTrackAudioFile::size(uint64_t& size) const {
#if defined(_WIN32)
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(handle_, &value))
        return false;
    size = static_cast<uint64_t>(value.QuadPart);
#else
    struct stat status {};
    if (::fstat(fd_, &status) != 0)
        return false;
    size = static_cast<uint64_t>(status.st_size);
#endif
    return true;
}

bool FrozenTrackAudioFile::rename(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto name = path.wstring();
    std::vector<uint8_t> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.data(), info->FileNameLength);
    if (!SetFileInformationByHandle(handle_, FileRenameInfo, info, static_cast<DWORD>(buffer.size())))
        return false;
#else
    if (::rename(path_.c_str(), path.c_str()) != 0)
        return false;
#endif
    path_ = path;
    return true;
}

std::shared_ptr<FrozenTrackAudio> FrozenTrackAudio::create(
    const std::filesystem::path& directory,
    std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = std::format("Cannot create the frozen track directory {}: {}", directory.string(), ec.message());
        return nullptr;
    }
    std::shared_ptr<FrozenTrackAudio> audio(new FrozenTrackAudio());
    audio->token_ = newToken();
    audio->directory_ = directory;
    audio->file_ = FrozenTrackAudioFile::create(
        directory / (audio->token_ + std::string(kTemporaryExtension)), error);
    if (!audio->file_)
        return nullptr;
    audio->file_->removeOnClose(true);
    return audio;
}

std::shared_ptr<FrozenTrackAudio> FrozenTrackAudio::open(
    const std::filesystem::path& directory,
    const std::string& token,
    std::string& error) {
    if (!isToken(token)) {
        error = "The frozen track reference is malformed.";
        return nullptr;
    }
    std::shared_ptr<FrozenTrackAudio> audio(new FrozenTrackAudio());
    audio->token_ = token;
    audio->directory_ = directory;
    audio->persisted_ = true;
    audio->file_ = FrozenTrackAudioFile::open(directory / (token + std::string(kStoredExtension)), error);
    if (!audio->file_ || !audio->readTrailer(error))
        return nullptr;
    audio->preparePlayback();
    // Recently used, as far as collectGarbage() is concerned.
    std::error_code ec;
    std::filesystem::last_write_time(audio->file_->path(), std::filesystem::file_time_type::clock::now(), ec);
    return audio;
}

void FrozenTrackAudio::removeAbandonedFiles(const std::filesystem::path& directory) {
    collectGarbage(directory, std::numeric_limits<uint64_t>::max(), {});
}

void FrozenTrackAudio::collectGarbage(
    const std::filesystem::path& directory,
    uint64_t limitBytes,
    const std::vector<std::string>& keptTokens) {
#if defined(__EMSCRIPTEN__)
    (void) directory;
    (void) limitBytes;
    (void) keptTokens;
#else
    struct Stored {
        std::filesystem::path path;
        uint64_t size;
        std::filesystem::file_time_type time;
    };
    std::vector<Stored> stored;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& path = it->path();
        const auto extension = path.extension().string();
        if (extension == kTemporaryExtension) {
            FrozenTrackAudioFile::removeUnlessHeld(path);
            continue;
        }
        if (extension != kStoredExtension || std::ranges::find(keptTokens, path.stem().string()) != keptTokens.end())
            continue;
        std::error_code entryError;
        const auto size = it->file_size(entryError);
        const auto time = it->last_write_time(entryError);
        if (!entryError)
            stored.push_back({path, size, time});
    }
    std::ranges::sort(stored, [](const Stored& a, const Stored& b) { return a.time > b.time; });
    uint64_t total = 0;
    for (const auto& file : stored) {
        if (total + file.size <= limitBytes || !FrozenTrackAudioFile::removeUnlessHeld(file.path))
            total += file.size;
    }
#endif
}

bool FrozenTrackAudio::writeFrames(
    const float* const* channels,
    uint32_t channelCount,
    uint32_t frames,
    std::string& error) {
    if (channel_count_ == 0) {
        if (channelCount == 0) {
            error = "The rendered track has no audio channel.";
            return false;
        }
        channel_count_ = channelCount;
        staging_.assign(static_cast<size_t>(channel_count_) * kChunkFrames, 0.0f);
    }
    if (channelCount != channel_count_) {
        error = "The rendered track changed its channel count.";
        return false;
    }
    uint32_t done = 0;
    while (done < frames) {
        const auto count = std::min(frames - done, kChunkFrames - staging_frames_);
        for (uint32_t channel = 0; channel < channel_count_; ++channel)
            std::memcpy(
                staging_.data() + static_cast<size_t>(channel) * kChunkFrames + staging_frames_,
                channels[channel] + done,
                count * sizeof(float));
        staging_frames_ += count;
        rendered_frames_ += count;
        done += count;
        if (staging_frames_ == kChunkFrames && !flushChunk(error))
            return false;
    }
    return true;
}

bool FrozenTrackAudio::flushChunk(std::string& error) {
    if (staging_frames_ == 0)
        return true;
    for (uint32_t channel = 0; channel < channel_count_; ++channel)
        std::fill_n(
            staging_.data() + static_cast<size_t>(channel) * kChunkFrames + staging_frames_,
            kChunkFrames - staging_frames_,
            0.0f);
    StoredChunk stored{write_offset_, 0};
    if (!std::ranges::all_of(staging_, isSilent)) {
        encodeChunk(staging_.data(), channel_count_, kChunkFrames, encoding_);
        if (!file_->writeAt(write_offset_, encoding_.data(), encoding_.size())) {
            error = "Failed to write the frozen track audio to disk.";
            return false;
        }
        stored.size = static_cast<uint32_t>(encoding_.size());
        write_offset_ += encoding_.size();
        largest_stored_chunk_ = std::max(largest_stored_chunk_, stored.size);
    }
    stored_chunks_.push_back(stored);
    staging_frames_ = 0;
    return true;
}

bool FrozenTrackAudio::setChannelShifts(
    const std::vector<uint32_t>& busChannelCounts,
    const std::vector<int64_t>& busLatencyShifts,
    std::string& error) {
    if (busChannelCounts.size() != busLatencyShifts.size()) {
        error = "The track render reported an inconsistent bus layout.";
        return false;
    }
    channel_shifts_.clear();
    for (size_t bus = 0; bus < busChannelCounts.size(); ++bus) {
        if (busChannelCounts[bus] > channel_count_ - channel_shifts_.size()) {
            error = "The track render reported an inconsistent channel count.";
            return false;
        }
        channel_shifts_.insert(channel_shifts_.end(), busChannelCounts[bus], busLatencyShifts[bus]);
    }
    if (channel_shifts_.size() != channel_count_) {
        error = "The track render reported an inconsistent channel count.";
        return false;
    }
    bus_channel_counts_ = busChannelCounts;
    bus_latency_shifts_ = busLatencyShifts;
    return true;
}

bool FrozenTrackAudio::finishWriting(
    int64_t startSample,
    const std::vector<uint32_t>& busChannelCounts,
    const std::vector<int64_t>& busLatencyShifts,
    FrozenTrackAudioIdentity identity,
    std::string& error) {
    if (!flushChunk(error))
        return false;
    staging_ = {};
    encoding_ = {};
    if (channel_count_ == 0 || rendered_frames_ <= 0) {
        error = "The track rendered no audio.";
        return false;
    }
    if (!setChannelShifts(busChannelCounts, busLatencyShifts, error))
        return false;
    start_sample_ = startSample;
    identity_ = std::move(identity);
    chunk_count_ = static_cast<int64_t>(stored_chunks_.size());
    if (!writeTrailer(error))
        return false;
    preparePlayback();
    return true;
}

bool FrozenTrackAudio::writeTrailer(std::string& error) {
    ByteWriter trailer;
    appendBytes(trailer.bytes, kTrailerMagic, sizeof(kTrailerMagic));
    trailer.value(kFormatVersion);
    trailer.string(token_);
    trailer.value(identity_.sampleRate);
    trailer.value(start_sample_);
    trailer.value(rendered_frames_);
    trailer.value(channel_count_);
    trailer.value(static_cast<uint32_t>(bus_channel_counts_.size()));
    for (size_t bus = 0; bus < bus_channel_counts_.size(); ++bus) {
        trailer.value(bus_channel_counts_[bus]);
        trailer.value(bus_latency_shifts_[bus]);
    }
    trailer.value(static_cast<uint32_t>(identity_.busLatencies.size()));
    for (const auto latency : identity_.busLatencies)
        trailer.value(latency);
    trailer.value(static_cast<uint32_t>(identity_.plugins.size()));
    for (const auto& plugin : identity_.plugins)
        trailer.string(plugin);
    trailer.value(static_cast<uint64_t>(stored_chunks_.size()));
    for (const auto& chunk : stored_chunks_) {
        trailer.value(chunk.offset);
        trailer.value(chunk.size);
    }
    const auto trailerSize = static_cast<uint32_t>(trailer.bytes.size());
    trailer.value(write_offset_);
    trailer.value(trailerSize);
    appendBytes(trailer.bytes, kTrailerMagic, sizeof(kTrailerMagic));
    if (!file_->writeAt(write_offset_, trailer.bytes.data(), trailer.bytes.size())) {
        error = "Failed to write the frozen track audio to disk.";
        return false;
    }
    return true;
}

bool FrozenTrackAudio::readTrailer(std::string& error) {
    error = "The frozen track file is damaged.";
    uint64_t fileSize = 0;
    if (!file_->size(fileSize) || fileSize < kFooterSize)
        return false;
    uint8_t footer[kFooterSize];
    if (!file_->readAt(fileSize - kFooterSize, footer, kFooterSize))
        return false;
    ByteReader footerReader(footer, kFooterSize);
    const auto trailerOffset = footerReader.value<uint64_t>();
    const auto trailerSize = footerReader.value<uint32_t>();
    if (std::memcmp(footer + kFooterSize - sizeof(kTrailerMagic), kTrailerMagic, sizeof(kTrailerMagic)) != 0 ||
        trailerSize > kMaximumTrailerSize || trailerOffset > fileSize ||
        trailerOffset + trailerSize + kFooterSize != fileSize)
        return false;
    std::vector<uint8_t> bytes(trailerSize);
    if (!file_->readAt(trailerOffset, bytes.data(), bytes.size()))
        return false;
    ByteReader reader(bytes.data(), bytes.size());
    char magic[sizeof(kTrailerMagic)];
    for (auto& c : magic)
        c = reader.value<char>();
    if (!reader.ok() || std::memcmp(magic, kTrailerMagic, sizeof(magic)) != 0 ||
        reader.value<uint32_t>() != kFormatVersion || reader.string() != token_)
        return false;
    identity_.sampleRate = reader.value<int32_t>();
    start_sample_ = reader.value<int64_t>();
    rendered_frames_ = reader.value<int64_t>();
    channel_count_ = reader.value<uint32_t>();
    const auto busCount = reader.value<uint32_t>();
    if (!reader.fits(busCount, sizeof(uint32_t) + sizeof(int64_t)))
        return false;
    std::vector<uint32_t> busChannelCounts(busCount);
    std::vector<int64_t> busLatencyShifts(busCount);
    for (uint32_t bus = 0; bus < busCount; ++bus) {
        busChannelCounts[bus] = reader.value<uint32_t>();
        busLatencyShifts[bus] = reader.value<int64_t>();
    }
    const auto latencyCount = reader.value<uint32_t>();
    if (!reader.fits(latencyCount, sizeof(uint32_t)))
        return false;
    identity_.busLatencies.resize(latencyCount);
    for (auto& latency : identity_.busLatencies)
        latency = reader.value<uint32_t>();
    const auto pluginCount = reader.value<uint32_t>();
    if (!reader.fits(pluginCount, sizeof(uint32_t)))
        return false;
    identity_.plugins.resize(pluginCount);
    for (auto& plugin : identity_.plugins)
        plugin = reader.string();
    const auto chunkCount = reader.value<uint64_t>();
    if (!reader.fits(chunkCount, sizeof(uint64_t) + sizeof(uint32_t)))
        return false;
    stored_chunks_.resize(static_cast<size_t>(chunkCount));
    for (auto& chunk : stored_chunks_) {
        chunk.offset = reader.value<uint64_t>();
        chunk.size = reader.value<uint32_t>();
        if (chunk.offset > trailerOffset || chunk.size > trailerOffset - chunk.offset)
            return false;
        largest_stored_chunk_ = std::max(largest_stored_chunk_, chunk.size);
    }
    chunk_count_ = static_cast<int64_t>(chunkCount);
    if (!reader.ok() || !reader.atEnd() || channel_count_ == 0 || rendered_frames_ <= 0 ||
        identity_.sampleRate <= 0 || chunk_count_ != (rendered_frames_ + kChunkFrames - 1) / kChunkFrames)
        return false;
    if (!setChannelShifts(busChannelCounts, busLatencyShifts, error))
        return false;
    error.clear();
    return true;
}

void FrozenTrackAudio::preparePlayback() {
    minimum_shift_ = *std::ranges::min_element(channel_shifts_);
    maximum_shift_ = *std::ranges::max_element(channel_shifts_);
    lookahead_chunks_ = std::max<int64_t>(
        2, static_cast<int64_t>(std::ceil(std::max(1, identity_.sampleRate) * kLookaheadSeconds / kChunkFrames)));
    // Enough slots for every chunk neededChunks() asks for, so that consecutive chunks never share a slot.
    const auto shiftSpanChunks = (maximum_shift_ - minimum_shift_ + kChunkFrames - 1) / kChunkFrames;
    const auto slotCount = std::min<int64_t>(chunk_count_, lookahead_chunks_ + shiftSpanChunks + 3);
    slot_count_ = static_cast<uint32_t>(slotCount);
    slots_ = std::make_unique<Slot[]>(slot_count_);
    for (uint32_t slot = 0; slot < slot_count_; ++slot)
        slots_[slot].samples.resize(static_cast<size_t>(channel_count_) * kChunkFrames, 0.0f);
    loader_encoded_.reserve(largest_stored_chunk_);
    blocking_encoded_.reserve(largest_stored_chunk_);
    blocking_samples_.resize(static_cast<size_t>(channel_count_) * kChunkFrames, 0.0f);
    blocking_chunk_ = -1;
}

bool FrozenTrackAudio::persist() {
#if defined(__EMSCRIPTEN__)
    // Nothing on this file system outlives the page.
    return false;
#else
    if (persisted_)
        return true;
    if (slot_count_ == 0 || !file_->rename(directory_ / (token_ + std::string(kStoredExtension))))
        return false;
    file_->removeOnClose(false);
    persisted_ = true;
    std::error_code ec;
    std::filesystem::last_write_time(file_->path(), std::filesystem::file_time_type::clock::now(), ec);
    return true;
#endif
}

bool FrozenTrackAudio::loadChunk(int64_t chunk, float* samples, std::vector<uint8_t>& encoded) const noexcept {
    const auto& stored = stored_chunks_[static_cast<size_t>(chunk)];
    if (stored.size == 0) {
        std::fill_n(samples, static_cast<size_t>(channel_count_) * kChunkFrames, 0.0f);
        return true;
    }
    if (encoded.capacity() < stored.size)
        return false;
    encoded.resize(stored.size);
    return file_->readAt(stored.offset, encoded.data(), stored.size) &&
        decodeChunk(encoded.data(), stored.size, samples, channel_count_, kChunkFrames);
}

void FrozenTrackAudio::neededChunks(int64_t position, int64_t& first, int64_t& last) const {
    first = std::max<int64_t>(0, floorDivide(position - maximum_shift_ - kChunkFrames, kChunkFrames));
    last = std::min<int64_t>(
        chunk_count_ - 1,
        floorDivide(position - minimum_shift_ + lookahead_chunks_ * kChunkFrames, kChunkFrames));
}

void FrozenTrackAudio::read(uint32_t channel, int64_t position, float* output, int32_t frames, bool waitForDisk) noexcept {
    if (channel >= channel_count_ || frames <= 0) {
        if (frames > 0)
            std::fill_n(output, frames, 0.0f);
        return;
    }
    const auto shift = channel_shifts_[channel];
    int32_t done = 0;
    while (done < frames) {
        const int64_t source = position + done - shift;
        auto count = frames - done;
        if (source < 0) {
            count = static_cast<int32_t>(std::min<int64_t>(count, -source));
            std::fill_n(output + done, count, 0.0f);
            done += count;
            continue;
        }
        if (source >= rendered_frames_) {
            std::fill_n(output + done, count, 0.0f);
            break;
        }
        const auto chunk = source / kChunkFrames;
        const auto offset = static_cast<uint32_t>(source % kChunkFrames);
        count = static_cast<int32_t>(std::min<int64_t>(
            {static_cast<int64_t>(count), static_cast<int64_t>(kChunkFrames - offset), rendered_frames_ - source}));
        const auto slot = static_cast<uint32_t>(chunk % slot_count_);
        bool copied = false;
        if (slots_[slot].chunk.load(std::memory_order_acquire) == chunk) {
            reading_slot_.store(static_cast<int32_t>(slot), std::memory_order_seq_cst);
            if (slots_[slot].chunk.load(std::memory_order_seq_cst) == chunk) {
                std::memcpy(output + done, chunkSamples(slot, channel) + offset, count * sizeof(float));
                copied = true;
            }
            reading_slot_.store(-1, std::memory_order_release);
        }
        if (!copied && waitForDisk) {
            if (blocking_chunk_ != chunk)
                blocking_chunk_ = loadChunk(chunk, blocking_samples_.data(), blocking_encoded_) ? chunk : -1;
            if (blocking_chunk_ == chunk) {
                std::memcpy(
                    output + done,
                    blocking_samples_.data() + static_cast<size_t>(channel) * kChunkFrames + offset,
                    count * sizeof(float));
                copied = true;
            }
        }
        // Not loaded yet, after a jump in position: silence rather than waiting for the disk.
        if (!copied)
            std::fill_n(output + done, count, 0.0f);
        done += count;
    }
}

bool FrozenTrackAudio::loadAhead() {
    if (slot_count_ == 0)
        return false;
    const auto position = play_position_.load(std::memory_order_acquire);
    int64_t first, last;
    neededChunks(position, first, last);
    if (last < first)
        return false;
    // The chunk being played first, then ahead of it, then the one just behind.
    const auto current = std::clamp<int64_t>(floorDivide(position - maximum_shift_, kChunkFrames), first, last);
    for (int64_t step = 0; step <= last - first; ++step) {
        const auto chunk = current + step <= last ? current + step : first + (current + step - last - 1);
        const auto slot = static_cast<uint32_t>(chunk % slot_count_);
        if (slots_[slot].chunk.load(std::memory_order_relaxed) == chunk)
            continue;
        slots_[slot].chunk.store(-1, std::memory_order_seq_cst);
        while (reading_slot_.load(std::memory_order_seq_cst) == static_cast<int32_t>(slot))
            std::this_thread::yield();
        if (!loadChunk(chunk, slots_[slot].samples.data(), loader_encoded_))
            return false;
        slots_[slot].chunk.store(chunk, std::memory_order_release);
        // One chunk per call, so that a jump in position is followed at once.
        return true;
    }
    return false;
}

FrozenTrackAudio::Summary FrozenTrackAudio::summarize() const {
    Summary summary;
    std::vector<float> buffer(static_cast<size_t>(channel_count_) * kChunkFrames);
    std::vector<uint8_t> encoded;
    encoded.reserve(largest_stored_chunk_);
    double sumOfSquares = 0.0;
    uint64_t samples = 0;
    for (const auto shift : channel_shifts_) {
        const auto frames = static_cast<uint64_t>(std::max<int64_t>(0, rendered_frames_ + shift));
        summary.frames = std::max(summary.frames, frames);
        samples += frames;
    }
    for (int64_t chunk = 0; chunk < chunk_count_; ++chunk) {
        if (!loadChunk(chunk, buffer.data(), encoded))
            break;
        for (uint32_t channel = 0; channel < channel_count_; ++channel) {
            // A negative shift drops the leading frames of a channel.
            const auto first = std::max<int64_t>(0, -channel_shifts_[channel]);
            for (uint32_t frame = 0; frame < kChunkFrames; ++frame) {
                const auto source = chunk * kChunkFrames + frame;
                if (source < first || source >= rendered_frames_)
                    continue;
                const auto sample = buffer[static_cast<size_t>(channel) * kChunkFrames + frame];
                summary.peak = std::max(summary.peak, std::abs(sample));
                sumOfSquares += static_cast<double>(sample) * sample;
            }
        }
    }
    summary.rms = samples ? std::sqrt(sumOfSquares / static_cast<double>(samples)) : 0.0;
    return summary;
}

} // namespace uapmd
