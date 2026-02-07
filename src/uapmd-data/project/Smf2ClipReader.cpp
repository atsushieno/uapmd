#include <uapmd-data/priv/project/Smf2ClipReader.hpp>
#include <uapmd-data/priv/project/SmfConverter.hpp>
#include <fstream>

namespace uapmd {

    Smf2ClipReader::ClipInfo Smf2ClipReader::read(const std::filesystem::path& file) {
        ClipInfo result;

        // TODO: Implement SMF2 (MIDI Clip File) reading using umppi
        // For now, delegate to readAnyFormat which handles all formats
        return readAnyFormat(file);
    }

    Smf2ClipReader::ClipInfo Smf2ClipReader::readAnyFormat(const std::filesystem::path& file) {
        ClipInfo result;

        if (!std::filesystem::exists(file)) {
            result.success = false;
            result.error = "File does not exist: " + file.string();
            return result;
        }

        // Check if it's a valid SMF file
        if (!isValidSmfFile(file)) {
            result.success = false;
            result.error = "Not a valid SMF file";
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
            result.error = "Failed to read SMF header";
            return result;
        }

        uint16_t format = (static_cast<uint8_t>(header[8]) << 8) | static_cast<uint8_t>(header[9]);

        // Check if it's SMF2 (MIDI Clip File)
        if (format == 2) {
            // SMF2 format - use native UMP reading
            // TODO: Implement direct SMF2 reading using umppi
            result.success = false;
            result.error = "SMF2 direct reading not yet implemented - use SMF0/1";
            return result;
        }

        // SMF0 or SMF1 - convert using SmfConverter
        auto convertResult = SmfConverter::convertToUmp(file);

        result.success = convertResult.success;
        result.error = convertResult.error;
        result.ump_data = std::move(convertResult.umpEvents);
        result.ump_tick_timestamps = std::move(convertResult.umpEventTicksStamps);
        result.tick_resolution = convertResult.tickResolution;
        result.tempo = convertResult.detectedTempo;

        return result;
    }

    bool Smf2ClipReader::isValidSmf2Clip(const std::filesystem::path& file) {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open())
            return false;

        char header[14];
        f.read(header, 14);
        if (!f)
            return false;

        // Check MThd header
        if (header[0] != 'M' || header[1] != 'T' || header[2] != 'h' || header[3] != 'd')
            return false;

        // Check format = 2 (SMF2)
        uint16_t format = (static_cast<uint8_t>(header[8]) << 8) | static_cast<uint8_t>(header[9]);
        return format == 2;
    }

    bool Smf2ClipReader::isValidSmfFile(const std::filesystem::path& file) {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open())
            return false;

        char header[4];
        f.read(header, 4);

        return (header[0] == 'M' && header[1] == 'T' &&
                header[2] == 'h' && header[3] == 'd');
    }

} // namespace uapmd
