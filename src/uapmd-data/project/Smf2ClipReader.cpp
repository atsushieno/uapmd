#include <fstream>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

namespace {

double tempoFromRawUmpValue(uint32_t rawTempo) {
    if (rawTempo == 0)
        return 120.0;
    return 6000000000.0 / static_cast<double>(rawTempo);
}

void ensureDefaultTempo(MidiClipReader::ClipInfo& clipInfo) {
    if (clipInfo.tempo_changes.empty()) {
        double fallbackBpm = clipInfo.tempo > 0.0 ? clipInfo.tempo : 120.0;
        clipInfo.tempo_changes.push_back(MidiTempoChange{0, fallbackBpm});
    }

    if (clipInfo.tempo_changes.front().bpm <= 0.0) {
        clipInfo.tempo_changes.front().bpm = 120.0;
    }

    if (clipInfo.tempo <= 0.0)
        clipInfo.tempo = clipInfo.tempo_changes.front().bpm;
}

void ensureDefaultTimeSignature(MidiClipReader::ClipInfo& clipInfo) {
    if (clipInfo.time_signature_changes.empty()) {
        clipInfo.time_signature_changes.push_back(MidiTimeSignatureChange{0, 4, 4});
    }
}

bool populateClipInfoFromSmf2Clip(const Smf2Clip& clip,
                                  MidiClipReader::ClipInfo& result,
                                  std::string& errorMessage) {
    auto fail = [&](const char* message) {
        errorMessage = message;
        return false;
    };

    if (clip.size() < 4)
        return fail("SMF2 clip is incomplete");

    auto it = clip.begin();
    if (!it->isDeltaClockstamp() || it->getDeltaClockstamp() != 0)
        return fail("SMF2 clip missing initial DeltaClockstamp(0)");
    ++it;

    if (!it->isDCTPQ())
        return fail("SMF2 clip missing DCTPQ after header");
    result.tick_resolution = it->getDCTPQ();
    ++it;

    if (!it->isDeltaClockstamp() || it->getDeltaClockstamp() != 0)
        return fail("SMF2 clip missing DeltaClockstamp before StartOfClip");
    ++it;

    if (!it->isStartOfClip())
        return fail("SMF2 clip missing StartOfClip marker");
    ++it;

    result.ump_data.clear();
    result.ump_tick_timestamps.clear();
    result.tempo_changes.clear();
    result.time_signature_changes.clear();
    result.tempo = 120.0;

    uint64_t currentTick = 0;
    bool expectDelta = true;
    bool endOfClipSeen = false;

    for (; it != clip.end(); ++it) {
        if (expectDelta) {
            if (!it->isDeltaClockstamp())
                return fail("SMF2 clip missing delta clockstamp");
            currentTick += it->getDeltaClockstamp();
            expectDelta = false;
            continue;
        }

        if (it->isEndOfClip()) {
            auto after = it;
            ++after;
            if (after != clip.end())
                return fail("EndOfClip must be the final event in SMF2 clip");
            endOfClipSeen = true;
            break;
        }

        if (it->isTempo()) {
            double bpm = tempoFromRawUmpValue(it->getTempo());
            result.tempo_changes.push_back(MidiTempoChange{currentTick, bpm});
            if (result.tempo_changes.size() == 1)
                result.tempo = bpm;
        } else if (it->isTimeSignature()) {
            MidiTimeSignatureChange sig{};
            sig.tickPosition = currentTick;
            sig.numerator = it->getTimeSignatureNumerator();
            sig.denominator = it->getTimeSignatureDenominator();
            result.time_signature_changes.push_back(sig);
        } else {
            const int byteCount = it->getSizeInBytes();
            const int words = byteCount / 4;
            const auto ints = it->toInts();
            for (int word = 0; word < words; ++word) {
                result.ump_data.push_back(ints[word]);
                result.ump_tick_timestamps.push_back(currentTick);
            }
        }

        expectDelta = true;
    }

    if (!endOfClipSeen)
        return fail("SMF2 clip missing EndOfClip marker");

    ensureDefaultTempo(result);
    ensureDefaultTimeSignature(result);
    result.success = true;
    result.error.clear();
    return true;
}

} // namespace

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

            if (!populateClipInfoFromSmf2Clip(*clipData, result, errorMessage)) {
                result.success = false;
                result.error = errorMessage;
                return result;
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
