#include <algorithm>
#include <chrono>
#include <format>
#include <filesystem>
#include <optional>

#include <umppi/umppi.hpp>
#include "uapmd-data/uapmd-data.hpp"

namespace uapmd::import {

MidiImportResult TrackImporter::importMidiFile(const std::string& filepath) {
    MidiImportResult result;
    result.success = false;

    try {
        const umppi::Midi1Music music = umppi::readMidi1File(filepath);
        if (music.tracks.empty()) {
            result.error = "The selected MIDI file contains no tracks.";
            return result;
        }

        const std::string baseFilename = std::filesystem::path(filepath).stem().string();
        for (size_t trackIdx = 0; trackIdx < music.tracks.size(); ++trackIdx) {
            auto convertResult = uapmd::SmfConverter::convertTrackToUmp(music, trackIdx);
            if (!convertResult.success) {
                result.warnings.push_back(std::format("Track {}: {}", trackIdx + 1, convertResult.error));
                continue;
            }

            if (convertResult.umpEvents.empty())
                continue;

            MidiTrackImport track;
            track.clipName = std::format("{} - Track {}", baseFilename, trackIdx + 1);
            track.umpEvents = std::move(convertResult.umpEvents);
            track.umpTickTimestamps = std::move(convertResult.umpEventTicksStamps);
            track.tickResolution = convertResult.tickResolution;
            track.detectedTempo = convertResult.detectedTempo;
            track.tempoChanges = std::move(convertResult.tempoChanges);
            track.timeSignatureChanges = std::move(convertResult.timeSignatureChanges);
            result.tracks.push_back(std::move(track));
        }

        if (result.tracks.empty()) {
            if (result.warnings.empty()) {
                result.error = "No MIDI tracks were imported.";
            }
            return result;
        }

        result.success = true;
        return result;
    } catch (const std::exception& ex) {
        result.error = std::format("Exception during SMF import:\n{}", ex.what());
        return result;
    }
}

AudioImportResult TrackImporter::importAudioFile(const std::string& filepath,
                                                 const AudioImportOptions& options) {
    AudioImportResult result;
    if (filepath.empty()) {
        result.error = "Audio file path is empty.";
        return result;
    }
    if (options.modelPath.empty()) {
        result.error = "Demucs model path is not specified.";
        return result;
    }

    std::filesystem::path modelPath(options.modelPath);
    if (!std::filesystem::exists(modelPath)) {
        result.error = "Demucs model path does not exist.";
        return result;
    }

    std::filesystem::path outputDir = options.outputDirectory;
    if (outputDir.empty()) {
        const auto timestamp = std::chrono::system_clock::now();
        const auto folderTag = std::format("{:%Y%m%d-%H%M%S}", timestamp);
        auto baseName = std::filesystem::path(filepath).stem().string();
        if (baseName.empty()) {
            baseName = "track";
        }
        outputDir = std::filesystem::temp_directory_path() / "uapmd-demucs" / std::format("{}-{}", baseName, folderTag);
    }

    DemucsStemSeparator separator(options.modelPath);
    auto separation = separator.separate(filepath, outputDir);
    if (!separation.success) {
        result.error = separation.error.empty() ? "Demucs failed to separate stems." : separation.error;
        return result;
    }

    auto baseName = std::filesystem::path(filepath).stem().string();
    if (baseName.empty()) {
        baseName = "stem";
    }

    for (const auto& stem : separation.stems) {
        AudioStemImport import;
        import.stemName = stem.name;
        import.filepath = stem.filepath;
        import.clipDisplayName = std::format("{} - {}", baseName, stem.name);
        result.stems.push_back(std::move(import));
    }

    result.success = !result.stems.empty();
    if (!result.success) {
        result.error = "No stems were imported.";
    }
    return result;
}

} // namespace uapmd::import
