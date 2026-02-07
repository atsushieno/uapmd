#pragma once

#include <vector>
#include <filesystem>
#include <cstdint>
#include <string>
#include <uapmd/uapmd.hpp>

namespace uapmd {

    // SMF (Standard MIDI File) to UMP converter
    // Converts SMF0/SMF1 files to UMP format using umppi library
    class SmfConverter {
    public:
        struct ConvertResult {
            bool success{false};
            std::string error;
            std::vector<uapmd_ump_t> umpEvents;
            std::vector<uint64_t> umpEventTicksStamps;  // Cumulative ticks for each UMP event
            uint32_t tickResolution{480};
            double detectedTempo{120.0};  // BPM from SMF tempo meta-events
        };

        // Convert any SMF format to UMP
        // Handles SMF Format 0 and Format 1 (merges tracks)
        static ConvertResult convertToUmp(const std::filesystem::path& smfFile);

    private:
        // Extract tempo from SMF meta-events (0xFF 0x51)
        // Returns tempo in BPM
        static double extractTempoFromSmf(const std::vector<uint8_t>& smfData);

        // Check if file is valid SMF
        static bool isValidSmfFile(const std::filesystem::path& file);
    };

} // namespace uapmd
