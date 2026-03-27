
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>
#include "uapmd-data/detail/project/ProjectArchive.hpp"

namespace uapmd {

namespace {

constexpr uint32_t kLocalHeaderSignature   = 0x04034b50;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50;
constexpr uint32_t kEndOfDirSignature      = 0x06054b50;
constexpr uint16_t kZipVersion             = 20; // ZIP 2.0
constexpr uint64_t kMaxEocdSearch          = 0xFFFF + 22; // comment + EOCD size

constexpr std::array<uint32_t, 256> kCrc32Table = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            if (c & 1)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        table[i] = c;
    }
    return table;
}();

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc = kCrc32Table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

void appendLE16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void appendLE32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

bool readLE16(std::istream& in, uint16_t& value)
{
    uint8_t bytes[2];
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!in)
        return false;
    value = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    return true;
}

bool readLE32(std::istream& in, uint32_t& value)
{
    uint8_t bytes[4];
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!in)
        return false;
    value = static_cast<uint32_t>(bytes[0] |
                                  (bytes[1] << 8) |
                                  (bytes[2] << 16) |
                                  (bytes[3] << 24));
    return true;
}

struct CentralDirectoryEntry {
    std::string name;
    uint32_t crc32{};
    uint32_t compressedSize{};
    uint32_t uncompressedSize{};
    uint32_t localHeaderOffset{};
    uint16_t modTime{};
    uint16_t modDate{};
    uint16_t compression{};
    bool isDirectory{false};
};

struct EndOfCentralDirectory {
    uint16_t totalEntries{};
    uint32_t centralDirectorySize{};
    uint32_t centralDirectoryOffset{};
};

std::optional<EndOfCentralDirectory> readEocd(std::ifstream& file)
{
    file.seekg(0, std::ios::end);
    const auto fileSize = static_cast<uint64_t>(file.tellg());
    if (fileSize < 22)
        return std::nullopt;

    const uint64_t searchSize = std::min<uint64_t>(fileSize, kMaxEocdSearch);
    std::vector<uint8_t> buffer(static_cast<size_t>(searchSize));
    file.seekg(static_cast<std::streamoff>(fileSize - searchSize), std::ios::beg);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(searchSize));
    if (!file)
        return std::nullopt;

    for (int64_t i = static_cast<int64_t>(searchSize) - 22; i >= 0; --i) {
        uint32_t signature = static_cast<uint32_t>(buffer[i]) |
                             (static_cast<uint32_t>(buffer[i + 1]) << 8) |
                             (static_cast<uint32_t>(buffer[i + 2]) << 16) |
                             (static_cast<uint32_t>(buffer[i + 3]) << 24);
        if (signature != kEndOfDirSignature)
            continue;

        EndOfCentralDirectory eocd{};
        eocd.totalEntries = static_cast<uint16_t>(buffer[i + 10] |
                                                  (buffer[i + 11] << 8));
        eocd.centralDirectorySize = static_cast<uint32_t>(
            buffer[i + 12] |
            (buffer[i + 13] << 8) |
            (buffer[i + 14] << 16) |
            (buffer[i + 15] << 24));
        eocd.centralDirectoryOffset = static_cast<uint32_t>(
            buffer[i + 16] |
            (buffer[i + 17] << 8) |
            (buffer[i + 18] << 16) |
            (buffer[i + 19] << 24));
        return eocd;
    }

    return std::nullopt;
}

bool sanitizeRelativePath(const std::filesystem::path& input)
{
    if (input.empty() || input.is_absolute())
        return false;
    for (const auto& part : input) {
        if (part == "..")
            return false;
    }
    return true;
}

} // namespace

bool ProjectArchive::isArchive(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    uint32_t signature = 0;
    if (!readLE32(file, signature))
        return false;
    return signature == kLocalHeaderSignature;
}

bool ProjectArchive::createArchive(const std::filesystem::path& sourceDir,
                                   std::vector<uint8_t>& outData,
                                   std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(sourceDir, ec) || !std::filesystem::is_directory(sourceDir, ec)) {
        error = "Project staging directory is missing.";
        return false;
    }

    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(sourceDir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) {
            error = ec.message();
            return false;
        }
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    }

    if (files.empty()) {
        error = "Project staging directory is empty.";
        return false;
    }

    std::sort(files.begin(), files.end());
    std::vector<CentralDirectoryEntry> centralEntries;
    centralEntries.reserve(files.size());

    const std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(sourceDir, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    std::array<char, 64 * 1024> buffer{};

    for (const auto& filePath : files) {
        std::filesystem::path relative;
        try {
            relative = std::filesystem::relative(filePath, normalizedRoot);
        } catch (const std::exception& ex) {
            error = ex.what();
            return false;
        }

        relative = relative.lexically_normal();
        if (!sanitizeRelativePath(relative)) {
            error = "Encountered an invalid relative path while creating archive.";
            return false;
        }

        const std::string name = relative.generic_string();
        if (name.empty()) {
            error = "Encountered a file with an empty relative path.";
            return false;
        }
        if (name.size() > std::numeric_limits<uint16_t>::max()) {
            error = "A path inside the project is too long for ZIP format.";
            return false;
        }

        std::ifstream fileStream(filePath, std::ios::binary);
        if (!fileStream) {
            error = "Failed to open " + filePath.string();
            return false;
        }

        uint32_t crc = 0xFFFFFFFFu;
        uint64_t size = 0;
        while (fileStream) {
            fileStream.read(buffer.data(), buffer.size());
            auto readBytes = static_cast<size_t>(fileStream.gcount());
            if (readBytes == 0)
                break;
            crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(buffer.data()), readBytes);
            size += readBytes;
        }
        if (!fileStream.eof()) {
            error = "Failed to read " + filePath.string();
            return false;
        }

        crc ^= 0xFFFFFFFFu;
        if (size > std::numeric_limits<uint32_t>::max()) {
            error = "A file in the project exceeds the 4 GiB ZIP limit.";
            return false;
        }

        const auto offset = outData.size();
        if (offset > std::numeric_limits<uint32_t>::max()) {
            error = "Archive grew beyond the ZIP 32-bit offset limit.";
            return false;
        }

        appendLE32(outData, kLocalHeaderSignature);
        appendLE16(outData, kZipVersion);
        appendLE16(outData, 0); // flags
        appendLE16(outData, 0); // compression (store)
        appendLE16(outData, 0); // mod time
        appendLE16(outData, 0); // mod date
        appendLE32(outData, crc);
        appendLE32(outData, static_cast<uint32_t>(size));
        appendLE32(outData, static_cast<uint32_t>(size));
        appendLE16(outData, static_cast<uint16_t>(name.size()));
        appendLE16(outData, 0); // extra length
        outData.insert(outData.end(), name.begin(), name.end());

        std::ifstream writer(filePath, std::ios::binary);
        if (!writer) {
            error = "Failed to reopen " + filePath.string();
            return false;
        }
        while (writer) {
            writer.read(buffer.data(), buffer.size());
            auto readBytes = static_cast<size_t>(writer.gcount());
            if (readBytes == 0)
                break;
            const auto* begin = reinterpret_cast<const uint8_t*>(buffer.data());
            outData.insert(outData.end(), begin, begin + readBytes);
        }
        if (!writer.eof()) {
            error = "Failed to stream " + filePath.string();
            return false;
        }

        CentralDirectoryEntry entry{};
        entry.name = name;
        entry.crc32 = crc;
        entry.compressedSize = static_cast<uint32_t>(size);
        entry.uncompressedSize = static_cast<uint32_t>(size);
        entry.localHeaderOffset = static_cast<uint32_t>(offset);
        entry.compression = 0;
        entry.isDirectory = false;
        centralEntries.push_back(std::move(entry));
    }

    const auto centralDirOffset = outData.size();
    if (centralDirOffset > std::numeric_limits<uint32_t>::max()) {
        error = "Archive grew beyond the ZIP 32-bit offset limit.";
        return false;
    }

    for (const auto& entry : centralEntries) {
        appendLE32(outData, kCentralHeaderSignature);
        appendLE16(outData, kZipVersion);
        appendLE16(outData, kZipVersion);
        appendLE16(outData, 0); // flags
        appendLE16(outData, entry.compression);
        appendLE16(outData, entry.modTime);
        appendLE16(outData, entry.modDate);
        appendLE32(outData, entry.crc32);
        appendLE32(outData, entry.compressedSize);
        appendLE32(outData, entry.uncompressedSize);
        appendLE16(outData, static_cast<uint16_t>(entry.name.size()));
        appendLE16(outData, 0); // extra
        appendLE16(outData, 0); // comment
        appendLE16(outData, 0); // disk number start
        appendLE16(outData, 0); // internal attrs
        appendLE32(outData, 0); // external attrs
        appendLE32(outData, entry.localHeaderOffset);
        outData.insert(outData.end(), entry.name.begin(), entry.name.end());
    }

    if (centralEntries.size() > std::numeric_limits<uint16_t>::max()) {
        error = "Archive has too many files for ZIP format.";
        return false;
    }

    const auto centralDirSize = outData.size() - centralDirOffset;
    appendLE32(outData, kEndOfDirSignature);
    appendLE16(outData, 0); // disk number
    appendLE16(outData, 0); // disk start
    appendLE16(outData, static_cast<uint16_t>(centralEntries.size()));
    appendLE16(outData, static_cast<uint16_t>(centralEntries.size()));
    appendLE32(outData, static_cast<uint32_t>(centralDirSize));
    appendLE32(outData, static_cast<uint32_t>(centralDirOffset));
    appendLE16(outData, 0); // comment length

    return true;
}

ProjectArchiveExtractResult ProjectArchive::extractArchive(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& destinationDir)
{
    ProjectArchiveExtractResult result;

    std::ifstream file(archivePath, std::ios::binary);
    if (!file) {
        result.error = "Failed to open archive: " + archivePath.string();
        return result;
    }

    auto eocd = readEocd(file);
    if (!eocd) {
        result.error = "Archive footer (EOCD) not found.";
        return result;
    }

    file.seekg(static_cast<std::streamoff>(eocd->centralDirectoryOffset), std::ios::beg);

    std::vector<CentralDirectoryEntry> entries;
    entries.reserve(eocd->totalEntries);

    for (uint16_t i = 0; i < eocd->totalEntries; ++i) {
        uint32_t signature = 0;
        if (!readLE32(file, signature) || signature != kCentralHeaderSignature) {
            result.error = "Corrupt central directory entry.";
            return result;
        }

        uint16_t versionMade = 0;
        uint16_t versionNeeded = 0;
        uint16_t flags = 0;
        uint16_t compression = 0;
        uint16_t modTime = 0;
        uint16_t modDate = 0;
        uint32_t crc = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t nameLen = 0;
        uint16_t extraLen = 0;
        uint16_t commentLen = 0;
        uint16_t diskStart = 0;
        uint16_t internalAttrs = 0;
        uint32_t externalAttrs = 0;
        uint32_t localOffset = 0;

        if (!readLE16(file, versionMade) ||
            !readLE16(file, versionNeeded) ||
            !readLE16(file, flags) ||
            !readLE16(file, compression) ||
            !readLE16(file, modTime) ||
            !readLE16(file, modDate) ||
            !readLE32(file, crc) ||
            !readLE32(file, compressedSize) ||
            !readLE32(file, uncompressedSize) ||
            !readLE16(file, nameLen) ||
            !readLE16(file, extraLen) ||
            !readLE16(file, commentLen) ||
            !readLE16(file, diskStart) ||
            !readLE16(file, internalAttrs) ||
            !readLE32(file, externalAttrs) ||
            !readLE32(file, localOffset)) {
            result.error = "Unexpected end of central directory.";
            return result;
        }

        std::string name(nameLen, '\0');
        file.read(name.data(), nameLen);
        if (!file) {
            result.error = "Failed to read filename from central directory.";
            return result;
        }

        file.seekg(extraLen + commentLen, std::ios::cur);
        if (!file) {
            result.error = "Failed to skip central directory metadata.";
            return result;
        }

        CentralDirectoryEntry entry{};
        entry.name = name;
        entry.crc32 = crc;
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.localHeaderOffset = localOffset;
        entry.modTime = modTime;
        entry.modDate = modDate;
        entry.compression = compression;
        entry.isDirectory = !name.empty() && name.back() == '/';
        entries.push_back(std::move(entry));
    }

    std::error_code dirEc;
    std::filesystem::create_directories(destinationDir, dirEc);
    if (dirEc) {
        result.error = "Failed to create extraction directory: " + dirEc.message();
        return result;
    }

    std::vector<char> buffer(64 * 1024);

    for (const auto& entry : entries) {
        std::filesystem::path relative = std::filesystem::path(entry.name).lexically_normal();
        if (entry.isDirectory) {
            if (!sanitizeRelativePath(relative)) {
                result.error = "Archive contains invalid directory path.";
                return result;
            }
            std::filesystem::create_directories(destinationDir / relative, dirEc);
            if (dirEc) {
                result.error = "Failed to create directory while extracting: " + dirEc.message();
                return result;
            }
            continue;
        }

        if (!sanitizeRelativePath(relative)) {
            result.error = "Archive contains invalid file path.";
            return result;
        }

        if (entry.compression != 0) {
            result.error = "Archive uses unsupported compression method.";
            return result;
        }

        file.seekg(static_cast<std::streamoff>(entry.localHeaderOffset), std::ios::beg);
        uint32_t localSig = 0;
        if (!readLE32(file, localSig) || localSig != kLocalHeaderSignature) {
            result.error = "Corrupt local header.";
            return result;
        }

        uint16_t versionNeeded = 0;
        uint16_t flags = 0;
        uint16_t compression = 0;
        uint16_t modTime = 0;
        uint16_t modDate = 0;
        uint32_t crc = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t fileNameLen = 0;
        uint16_t extraLen = 0;

        if (!readLE16(file, versionNeeded) ||
            !readLE16(file, flags) ||
            !readLE16(file, compression) ||
            !readLE16(file, modTime) ||
            !readLE16(file, modDate) ||
            !readLE32(file, crc) ||
            !readLE32(file, compressedSize) ||
            !readLE32(file, uncompressedSize) ||
            !readLE16(file, fileNameLen) ||
            !readLE16(file, extraLen)) {
            result.error = "Unexpected end of local header.";
            return result;
        }

        file.seekg(fileNameLen + extraLen, std::ios::cur);
        if (!file) {
            result.error = "Failed to skip local header data.";
            return result;
        }

        if (compressedSize != entry.compressedSize ||
            uncompressedSize != entry.uncompressedSize) {
            result.error = "Central directory mismatch detected.";
            return result;
        }

        std::filesystem::path outputPath = destinationDir / relative;
        std::filesystem::create_directories(outputPath.parent_path(), dirEc);
        if (dirEc) {
            result.error = "Failed to prepare extraction path: " + dirEc.message();
            return result;
        }

        std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
        if (!outFile) {
            result.error = "Failed to create " + outputPath.string();
            return result;
        }

        uint64_t remaining = entry.compressedSize;
        uint32_t runningCrc = 0xFFFFFFFFu;
        while (remaining > 0) {
            const auto chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
            file.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (static_cast<size_t>(file.gcount()) != chunk) {
                result.error = "Unexpected end of file while extracting.";
                return result;
            }
            outFile.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!outFile) {
                result.error = "Failed to write extracted data.";
                return result;
            }
            runningCrc = crc32Update(runningCrc, reinterpret_cast<const uint8_t*>(buffer.data()), chunk);
            remaining -= chunk;
        }
        runningCrc ^= 0xFFFFFFFFu;
        if (runningCrc != entry.crc32) {
            result.error = "CRC mismatch while extracting " + outputPath.string();
            return result;
        }

        if (outputPath.extension() == ".uapmd" && result.projectFile.empty())
            result.projectFile = outputPath;
    }

    if (result.projectFile.empty()) {
        result.error = "Archive did not contain a .uapmd project file.";
        return result;
    }

    result.success = true;
    return result;
}

} // namespace uapmd
