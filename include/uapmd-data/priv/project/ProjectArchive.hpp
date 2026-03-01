#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uapmd {

struct ProjectArchiveExtractResult {
    bool success{false};
    std::string error;
    std::filesystem::path projectFile; // Absolute path to the extracted .uapmd file
};

class ProjectArchive {
public:
    // Returns true if the file begins with a ZIP local-file header signature.
    static bool isArchive(const std::filesystem::path& path);

    // Packs every regular file under sourceDir (recursively) into a ZIP archive
    // using the "store" method (no compression). The resulting bytes are written
    // to outData. Returns false and fills error on failure.
    static bool createArchive(const std::filesystem::path& sourceDir,
                              std::vector<uint8_t>& outData,
                              std::string& error);

    // Extracts the archive at archivePath into destinationDir. The destination
    // directory must already exist. Returns the extracted project file path
    // (the first *.uapmd file encountered) in the result.
    static ProjectArchiveExtractResult extractArchive(
        const std::filesystem::path& archivePath,
        const std::filesystem::path& destinationDir);

private:
    ProjectArchive() = default;
};

} // namespace uapmd
