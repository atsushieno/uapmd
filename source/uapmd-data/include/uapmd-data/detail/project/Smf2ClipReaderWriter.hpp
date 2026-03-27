#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <umppi/umppi.hpp>

namespace uapmd {

    using Smf2Clip = std::vector<umppi::Ump>;

    class Smf2ClipReaderWriter {
    public:
        static std::optional<Smf2Clip> read(const std::filesystem::path& file, std::string* errorMessage = nullptr);
        static bool write(const std::filesystem::path& file, const Smf2Clip& clip, std::string* errorMessage = nullptr);

    private:
        static constexpr const char* kFileHeader = "SMF2CLIP";
        static bool validateHeader(const std::array<char, 8>& header);
        static bool validateClipBody(const Smf2Clip& clip, std::string* errorMessage);
        static void setError(std::string* target, const std::string& message);
    };

} // namespace uapmd
