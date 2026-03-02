#pragma once

#include <vector>
#include <filesystem>
#include <cstdint>
#include <string>
#include <uapmd/uapmd.hpp>
#include "../midi/MidiTimelineEvents.hpp"

namespace umppi {
    struct Midi1Music;
}

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
            std::vector<MidiTempoChange> tempoChanges;   // Ordered tempo changes with tick offsets
            std::vector<MidiTimeSignatureChange> timeSignatureChanges; // Ordered time signature changes
            uint32_t tickResolution{480};
            double detectedTempo{120.0};  // BPM from SMF tempo meta-events
        };

        // Convert any SMF format to UMP
        // Handles SMF Format 0 and Format 1 (merges tracks)
        static ConvertResult convertToUmp(const std::filesystem::path& smfFile);

        // Convert a single track from an SMF file to UMP
        // trackIndex: 0-based index of the track to convert
        static ConvertResult convertTrackToUmp(const std::filesystem::path& smfFile, size_t trackIndex);

        // Convert a single track from already-loaded MIDI music to UMP
        // music: Already loaded Midi1Music object
        // trackIndex: 0-based index of the track to convert
        static ConvertResult convertTrackToUmp(const umppi::Midi1Music& music, size_t trackIndex);

    private:
        // Extract tempo from SMF meta-events (0xFF 0x51)
        // Returns tempo in BPM
        static double extractTempoFromSmf(const std::vector<uint8_t>& smfData);

        // Check if file is valid SMF
        static bool isValidSmfFile(const std::filesystem::path& file);
    };

} // namespace uapmd
