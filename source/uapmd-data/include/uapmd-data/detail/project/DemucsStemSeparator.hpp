#pragma once

#include <filesystem>
#include <functional>
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
        bool canceled{false};
        std::string error;
        std::vector<StemFile> stems;
    };

    using ProgressCallback = std::function<bool(float /*progress*/, const std::string& /*message*/)>;  // return false to cancel
    using ShouldCancelCallback = std::function<bool()>;

    explicit DemucsStemSeparator(std::string modelPath);

    Result separate(const std::string& audioFile,
                    const std::filesystem::path& outputDir,
                    ProgressCallback progressCallback = {},
                    ShouldCancelCallback shouldCancel = {}) const;

private:
    std::string modelPath_;
};

} // namespace uapmd::import
