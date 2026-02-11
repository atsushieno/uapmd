#pragma once

#include <vector>
#include <filesystem>
#include <cstdint>
#include <uapmd/uapmd.hpp>
#include "../midi/MidiTimelineEvents.hpp"

namespace uapmd {
    // SMF2 Clip File reader according to M2-116-U v1.0 MIDI Clip File Specification
    class MidiClipReader {
    public:
        struct ClipInfo {
            uint32_t tick_resolution{};  // Ticks per quarter note
            std::vector<uapmd_ump_t> ump_data{};
            std::vector<uint64_t> ump_tick_timestamps{};  // Cumulative ticks for each UMP word
            std::vector<MidiTempoChange> tempo_changes;
            std::vector<MidiTimeSignatureChange> time_signature_changes;
            double tempo{120.0};         // Detected tempo in BPM
            bool success{true};          // Conversion success flag
            std::string error;           // Error message if failed
        };

        // Auto-detect SMF format and convert to UMP
        // Handles SMF0, SMF1, and SMF2 (MIDI Clip File) formats
        static ClipInfo readAnyFormat(const std::filesystem::path& file);

        // Check if a file is a valid SMF2 clip file
        static bool isValidSmf2Clip(const std::filesystem::path& file);

        // Check if a file is a valid SMF file (any format)
        static bool isValidSmfFile(const std::filesystem::path& file);
    };
}
