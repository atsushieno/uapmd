#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

namespace {

std::vector<uint8_t> readRemainingBytes(std::ifstream& stream) {
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

bool writeUmp(std::ofstream& stream, const umppi::Ump& ump) {
    const int byteCount = ump.getSizeInBytes();
    if (byteCount <= 0)
        return true;

    const int wordCount = byteCount / static_cast<int>(sizeof(uint32_t));
    std::array<uint8_t, 16> buffer{};
    const auto ints = ump.toInts();

    for (int word = 0; word < wordCount; ++word) {
        const uint32_t value = ints[word];
        const int offset = word * 4;
        buffer[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
        buffer[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        buffer[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        buffer[offset + 3] = static_cast<uint8_t>(value & 0xFF);
    }

    stream.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(byteCount));
    return stream.good();
}

} // namespace

bool Smf2ClipReaderWriter::validateHeader(const std::array<char, 8>& header) {
    return std::equal(header.begin(), header.end(), kFileHeader);
}

bool Smf2ClipReaderWriter::validateClipBody(const Smf2Clip& clip, std::string* errorMessage) {
    if (clip.size() < 4) {
        setError(errorMessage, "SMF2 Clip is missing required header UMPs");
        return false;
    }

    auto it = clip.begin();
    if (!it->isDeltaClockstamp() || it->getDeltaClockstamp() != 0) {
        setError(errorMessage, "First UMP must be DeltaClockstamp(0)");
        return false;
    }
    ++it;

    if (!it->isDCTPQ()) {
        setError(errorMessage, "Second UMP must be DCTPQ");
        return false;
    }
    ++it;

    if (!it->isDeltaClockstamp() || it->getDeltaClockstamp() != 0) {
        setError(errorMessage, "Third UMP must be DeltaClockstamp(0)");
        return false;
    }
    ++it;

    if (!it->isStartOfClip()) {
        setError(errorMessage, "Fourth UMP must be StartOfClip");
        return false;
    }
    ++it;

    while (it != clip.end()) {
        if (!it->isDeltaClockstamp()) {
            setError(errorMessage, "DeltaClockstamp expected before every event");
            return false;
        }
        ++it;
        if (it == clip.end()) {
            setError(errorMessage, "DeltaClockstamp without following event");
            return false;
        }
        ++it;
    }

    return true;
}

void Smf2ClipReaderWriter::setError(std::string* target, const std::string& message) {
    if (target)
        *target = message;
}

std::optional<Smf2Clip> Smf2ClipReaderWriter::read(const std::filesystem::path& file, std::string* errorMessage) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        setError(errorMessage, "Failed to open SMF2 Clip");
        return std::nullopt;
    }

    std::array<char, 8> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()) || !validateHeader(header)) {
        setError(errorMessage, "Invalid SMF2 Clip header");
        return std::nullopt;
    }

    const auto data = readRemainingBytes(input);
    if (data.empty() || (data.size() % sizeof(uint32_t)) != 0) {
        setError(errorMessage, "Corrupt SMF2 Clip payload");
        return std::nullopt;
    }

    Smf2Clip clip = umppi::Ump::fromBytes(data.data(), data.size());
    if (!validateClipBody(clip, errorMessage))
        return std::nullopt;

    return clip;
}

bool Smf2ClipReaderWriter::write(const std::filesystem::path& file, const Smf2Clip& clip, std::string* errorMessage) {
    if (!validateClipBody(clip, errorMessage))
        return false;

    std::ofstream output(file, std::ios::binary);
    if (!output) {
        setError(errorMessage, "Failed to create SMF2 Clip file");
        return false;
    }

    output.write(kFileHeader, 8);
    if (!output) {
        setError(errorMessage, "Failed to write SMF2 Clip header");
        return false;
    }

    for (const auto& ump : clip) {
        if (!writeUmp(output, ump)) {
            setError(errorMessage, "Failed to write SMF2 Clip data");
            return false;
        }
    }

    output.flush();
    if (!output) {
        setError(errorMessage, "Failed to finalize SMF2 Clip");
        return false;
    }

    return true;
}

} // namespace uapmd
