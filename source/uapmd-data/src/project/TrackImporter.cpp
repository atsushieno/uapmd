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

            const std::string fallbackTrackName = std::format("{} - Track {}", baseFilename, trackIdx + 1);
            const std::string trackClipName = convertResult.clipName.empty()
                ? fallbackTrackName
                : convertResult.clipName;

            MidiTrackImport track;
            track.clipName = trackClipName;
            track.umpEvents = std::move(convertResult.umpEvents);
            track.umpTickTimestamps = std::move(convertResult.umpEventTicksStamps);
            track.tickResolution = convertResult.tickResolution;
            track.detectedTempo = convertResult.detectedTempo;
            track.tempoChanges = std::move(convertResult.tempoChanges);
            track.timeSignatureChanges = std::move(convertResult.timeSignatureChanges);

            // Every track whose *own* raw events embed tempo/time-signature meta gets its own
            // master clip (not just the first one in the file). Note: hasExplicitTempoChanges/
            // hasExplicitTimeSignatureChanges are collected file-wide (tempo/time-signature
            // events may reside on any track) and would be true for every track in a file that
            // has tempo data anywhere -- hasOwnExplicit* is scoped to this specific track, which
            // is what determines whether *this* track should contribute a master clip.
            std::optional<MidiTrackImport> masterTrackClip;
            if (convertResult.hasOwnExplicitTempoChanges || convertResult.hasOwnExplicitTimeSignatureChanges) {
                masterTrackClip.emplace();
                masterTrackClip->clipName = std::format("{} Meta", trackClipName);
                masterTrackClip->tickResolution = track.tickResolution;
                masterTrackClip->detectedTempo = track.detectedTempo;
                if (convertResult.hasOwnExplicitTempoChanges)
                    masterTrackClip->tempoChanges = track.tempoChanges;
                if (convertResult.hasOwnExplicitTimeSignatureChanges)
                    masterTrackClip->timeSignatureChanges = track.timeSignatureChanges;
            }

            // The regular track's own copy becomes flat -- the curve now belongs to the master
            // clip only (see MidiClipReader::separateMasterTrackEvents for the same invariant on
            // the other import paths).
            uapmd::MidiClipReader::stripToFlatTempo(track.tempoChanges, track.timeSignatureChanges, track.detectedTempo);

            const bool trackHasEvents = !track.umpEvents.empty();
            if (trackHasEvents)
                result.tracks.push_back(std::move(track));

            if (masterTrackClip) {
                // Anchor to the just-created regular clip only if this track actually produced
                // one (a pure meta/conductor track with no note events has nothing to anchor to,
                // and keeps sourceTrackIndex == -1).
                if (trackHasEvents)
                    masterTrackClip->sourceTrackIndex = static_cast<int32_t>(result.tracks.size() - 1);
                result.masterTrackClips.push_back(std::move(*masterTrackClip));
            }
        }

        if (result.tracks.empty() && result.masterTrackClips.empty()) {
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

    auto reportProgress = [&](float value, const std::string& message) {
        if (options.progressCallback)
            options.progressCallback(value, message);
    };
    auto checkCanceled = [&]() -> bool {
        if (options.shouldCancel && options.shouldCancel()) {
            result.canceled = true;
            return true;
        }
        return false;
    };

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

    if (checkCanceled())
        return result;

    DemucsStemSeparator separator(options.modelPath);
    reportProgress(0.0f, "Preparing Demucs input...");
    auto separation = separator.separate(
        filepath,
        outputDir,
        [reportProgress](float value, const std::string& message) {
            reportProgress(value, message);
            return true;
        },
        options.shouldCancel);

    if (separation.canceled) {
        result.canceled = true;
        return result;
    }

    if (!separation.success) {
        result.error = separation.error.empty() ? "Demucs failed to separate stems." : separation.error;
        return result;
    }

    auto baseName = std::filesystem::path(filepath).stem().string();
    if (baseName.empty()) {
        baseName = "stem";
    }

    for (const auto& stem : separation.stems) {
        if (checkCanceled()) {
            result.stems.clear();
            return result;
        }
        AudioStemImport import;
        import.stemName = stem.name;
        import.filepath = stem.filepath;
        import.clipDisplayName = std::format("{} - {}", baseName, stem.name);
        result.stems.push_back(std::move(import));
    }

    result.success = !result.stems.empty();
    if (!result.success) {
        result.error = "No stems were imported.";
    } else {
        reportProgress(1.0f, "Import ready");
    }
    return result;
}

} // namespace uapmd::import
