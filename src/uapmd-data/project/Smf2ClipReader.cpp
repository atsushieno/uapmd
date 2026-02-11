#include <fstream>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

    MidiClipReader::ClipInfo MidiClipReader::readAnyFormat(const std::filesystem::path& file) {
        ClipInfo result;

        if (!std::filesystem::exists(file)) {
            result.success = false;
            result.error = "File does not exist: " + file.string();
            return result;
        }

        // Read file header to determine format
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open()) {
            result.success = false;
            result.error = "Failed to open file";
            return result;
        }

        char header[14];
        f.read(header, 14);
        if (!f) {
            result.success = false;
            result.error = "Failed to read file header";
            return result;
        }

        // Check if it's SMF2 (MIDI 2.0 Clip File) with "SMF2CLIP" header
        if (header[0] == 'S' && header[1] == 'M' && header[2] == 'F' &&
            header[3] == '2' && header[4] == 'C' && header[5] == 'L' &&
            header[6] == 'I' && header[7] == 'P') {
            // SMF2 (MIDI 2.0 Clip File) format - use native UMP reading
            f.close();
            std::string errorMessage;
            auto clipData = Smf2ClipReaderWriter::read(file, &errorMessage);
            if (!clipData) {
                result.success = false;
                result.error = errorMessage;
                return result;
            }

            // Convert Smf2Clip to ClipInfo format
            // TODO: Extract tick resolution and tempo from clip header
            result.success = true;
            result.tick_resolution = 480; // Default, should be extracted from DCTPQ
            result.tempo = 120.0; // Default, should be extracted from tempo events
            for (const auto& ump : *clipData) {
                auto ints = ump.toInts();
                int words = ump.getSizeInBytes() / 4;
                for (int i = 0; i < words; ++i)
                    result.ump_data.push_back(ints[i]);
            }
            return result;
        }

        // Check if it's a traditional SMF file (MThd header)
        if (header[0] != 'M' || header[1] != 'T' || header[2] != 'h' || header[3] != 'd') {
            result.success = false;
            result.error = "Not a valid MIDI file (expected MThd or SMF2CLIP header)";
            return result;
        }

        // Traditional SMF file (Format 0, 1, or 2) - convert using SmfConverter
        // Note: Format 2 here means "SMF Format 2" (multiple independent tracks),
        // NOT "SMF2" (MIDI 2.0 Clip File) which has a completely different header
        f.close();
        auto convertResult = SmfConverter::convertToUmp(file);

        result.success = convertResult.success;
        result.error = convertResult.error;
        result.ump_data = std::move(convertResult.umpEvents);
        result.ump_tick_timestamps = std::move(convertResult.umpEventTicksStamps);
        result.tick_resolution = convertResult.tickResolution;
        result.tempo_changes = std::move(convertResult.tempoChanges);
        result.time_signature_changes = std::move(convertResult.timeSignatureChanges);
        result.tempo = convertResult.detectedTempo;

        return result;
    }

    bool MidiClipReader::isValidSmf2Clip(const std::filesystem::path& file) {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open())
            return false;

        char header[8];
        f.read(header, 8);
        if (!f)
            return false;

        // Check for "SMF2CLIP" header (MIDI 2.0 Clip File per M2-116-U v1.0)
        // This is NOT the same as SMF Format 2 (which has "MThd" header)
        return (header[0] == 'S' && header[1] == 'M' && header[2] == 'F' &&
                header[3] == '2' && header[4] == 'C' && header[5] == 'L' &&
                header[6] == 'I' && header[7] == 'P');
    }

    bool MidiClipReader::isValidSmfFile(const std::filesystem::path& file) {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open())
            return false;

        char header[4];
        f.read(header, 4);

        // Check for traditional SMF (Standard MIDI File) header "MThd"
        // This covers SMF Format 0, 1, and 2 (all traditional MIDI 1.0 files)
        // NOT to be confused with SMF2 (MIDI 2.0 Clip File) which has "SMF2CLIP" header
        return (header[0] == 'M' && header[1] == 'T' &&
                header[2] == 'h' && header[3] == 'd');
    }

} // namespace uapmd
