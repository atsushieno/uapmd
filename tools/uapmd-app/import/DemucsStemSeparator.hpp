#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uapmd::import {

class DemucsStemSeparator {
public:
    struct StemFile {
        std::string name;
        std::filesystem::path filepath;
    };

    struct Result {
        bool success{false};
        std::string error;
        std::vector<StemFile> stems;
    };

    explicit DemucsStemSeparator(std::string modelPath);

    Result separate(const std::string& audioFile,
                    const std::filesystem::path& outputDir) const;

private:
    std::string modelPath_;
};

} // namespace uapmd::import
