#include <algorithm>
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
    const auto bytes = ump.toBytes();
    if (bytes.empty())
        return true;

    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
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
