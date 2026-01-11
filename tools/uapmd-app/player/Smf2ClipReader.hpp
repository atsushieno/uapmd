#pragma once

#include <vector>
#include <filesystem>
#include <cstdint>
#include <uapmd/uapmd.hpp>

namespace uapmd {
    // SMF2 Clip File reader according to M2-116-U v1.0 MIDI Clip File Specification
    class Smf2ClipReader {
    public:
        struct ClipInfo {
            uint32_t tick_resolution{};  // Ticks per quarter note
            std::vector<uapmd_ump_t> ump_data{};
        };

        // Read an SMF2 clip file and return UMP data
        static ClipInfo read(const std::filesystem::path& file);

        // Check if a file is a valid SMF2 clip file
        static bool isValidSmf2Clip(const std::filesystem::path& file);
    };
}
