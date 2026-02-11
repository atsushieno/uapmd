#pragma once

#include <vector>
#include <filesystem>
#include <cstdint>
#include <uapmd/uapmd.hpp>
#include "../midi/MidiTimelineEvents.hpp"

namespace uapmd {
    // MIDI Clip File reader supporting both traditional SMF and MIDI 2.0 Clip Files
    // IMPORTANT: "SMF2" (MIDI 2.0 Clip File) is DIFFERENT from "SMF Format 2":
    //   - SMF2 (MIDI 2.0 Clip File): M2-116-U v1.0, header "SMF2CLIP", contains UMP
    //   - SMF Format 2: Traditional MIDI 1.0, header "MThd", format type 2 (independent tracks)
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

        // Auto-detect MIDI file format and convert to UMP
        // Handles:
        //  - Traditional SMF (Format 0, 1, 2) with "MThd" header
        //  - SMF2 (MIDI 2.0 Clip File) with "SMF2CLIP" header per M2-116-U v1.0
        static ClipInfo readAnyFormat(const std::filesystem::path& file);

        // Check if a file is a valid SMF2 (MIDI 2.0 Clip File) with "SMF2CLIP" header
        // NOT the same as traditional SMF Format 2 which has "MThd" header
        static bool isValidSmf2Clip(const std::filesystem::path& file);

        // Check if a file is a valid traditional SMF (Standard MIDI File) with "MThd" header
        // Covers Format 0, 1, and 2 (all traditional MIDI 1.0 files)
        static bool isValidSmfFile(const std::filesystem::path& file);
    };
}
