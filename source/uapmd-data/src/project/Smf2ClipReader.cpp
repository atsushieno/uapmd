#include <cmath>
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

bool extractFlexTempo(const umppi::Ump& ump, double& bpm) {
    if (ump.getMessageType() != umppi::MessageType::FLEX_DATA)
        return false;

    const auto address = static_cast<uint8_t>((ump.getStatusByte() >> 4) & 0xF);
    const auto statusBank = static_cast<uint8_t>((ump.int1 >> 8) & 0xFF);
    const auto status = static_cast<uint8_t>(ump.int1 & 0xFF);
    if (address != umppi::FlexDataAddress::GROUP ||
        statusBank != umppi::FlexDataStatusBank::SETUP_AND_PERFORMANCE ||
        status != umppi::FlexDataStatus::TEMPO) {
        return false;
    }

    bpm = tempoFromRawUmpValue(ump.int2);
    return true;
}

bool extractFlexTimeSignature(const umppi::Ump& ump, MidiTimeSignatureChange& sig) {
    if (ump.getMessageType() != umppi::MessageType::FLEX_DATA)
        return false;

    const auto address = static_cast<uint8_t>((ump.getStatusByte() >> 4) & 0xF);
    const auto statusBank = static_cast<uint8_t>((ump.int1 >> 8) & 0xFF);
    const auto status = static_cast<uint8_t>(ump.int1 & 0xFF);
    if (address != umppi::FlexDataAddress::GROUP ||
        statusBank != umppi::FlexDataStatusBank::SETUP_AND_PERFORMANCE ||
        status != umppi::FlexDataStatus::TIME_SIGNATURE) {
        return false;
    }

    sig.numerator = static_cast<uint8_t>((ump.int2 >> 24) & 0xFF);
    sig.denominator = static_cast<uint8_t>((ump.int2 >> 16) & 0xFF);
    return true;
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
    result.has_explicit_tempo_changes = false;
    result.has_explicit_time_signature_changes = false;
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

        double flexTempo = 0.0;
        MidiTimeSignatureChange flexSig{};
        if (it->isTempo() || extractFlexTempo(*it, flexTempo)) {
            double bpm = it->isTempo() ? tempoFromRawUmpValue(it->getTempo()) : flexTempo;
            result.tempo_changes.push_back(MidiTempoChange{currentTick, bpm});
            result.has_explicit_tempo_changes = true;
            if (result.tempo_changes.size() == 1)
                result.tempo = bpm;
        } else if (it->isTimeSignature() || extractFlexTimeSignature(*it, flexSig)) {
            MidiTimeSignatureChange sig{};
            sig.tickPosition = currentTick;
            if (it->isTimeSignature()) {
                sig.numerator = it->getTimeSignatureNumerator();
                sig.denominator = it->getTimeSignatureDenominator();
            } else {
                sig.numerator = flexSig.numerator;
                sig.denominator = flexSig.denominator;
            }
            result.time_signature_changes.push_back(sig);
            result.has_explicit_time_signature_changes = true;
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

bool clipHasMeaningfulTempoMap(const MidiClipSourceNode& node) {
    const auto& tempoChanges = node.tempoChanges();
    if (tempoChanges.size() > 1)
        return true;
    if (!tempoChanges.empty() && std::abs(tempoChanges.front().bpm - 120.0) > 1.0e-6)
        return true;
    return false;
}

bool clipHasMeaningfulTimeSignatureMap(const MidiClipSourceNode& node) {
    const auto& changes = node.timeSignatureChanges();
    if (changes.size() > 1)
        return true;
    if (!changes.empty() &&
        (changes.front().numerator != 4 || changes.front().denominator != 4))
        return true;
    return false;
}

// Finds the first clip on the master track carrying a "meaningful" tempo/time-signature map, by
// clipId order. Regular tracks are never searched -- see MidiClipReader::stripToFlatTempo.
MidiClipSourceNode* findAuthoritativeTimelineMetaSource(const std::shared_ptr<TimelineTrack>& masterTrack) {
    if (!masterTrack)
        return nullptr;

    auto clips = masterTrack->clipManager().getAllClips();
    std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
        return a.clipId < b.clipId;
    });

    for (const auto& clip : clips) {
        if (clip.clipType != ClipType::Midi)
            continue;
        auto sourceNode = masterTrack->getSourceNode(clip.sourceNodeInstanceId);
        auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
        if (!midiNode)
            continue;
        if (clipHasMeaningfulTempoMap(*midiNode) || clipHasMeaningfulTimeSignatureMap(*midiNode))
            return midiNode;
    }
    return nullptr;
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
        result.has_explicit_tempo_changes = convertResult.hasExplicitTempoChanges;
        result.has_explicit_time_signature_changes = convertResult.hasExplicitTimeSignatureChanges;
        result.tempo = convertResult.detectedTempo;

        return result;
    }

    MidiClipReader::SeparatedMasterTrackEvents MidiClipReader::separateMasterTrackEvents(ClipInfo clipInfo) {
        SeparatedMasterTrackEvents result;
        result.musicalClip = clipInfo;

        result.masterTrackClip.tick_resolution = clipInfo.tick_resolution;
        result.masterTrackClip.tempo = clipInfo.tempo;
        result.masterTrackClip.success = clipInfo.success;
        result.masterTrackClip.has_explicit_tempo_changes = clipInfo.has_explicit_tempo_changes;
        result.masterTrackClip.has_explicit_time_signature_changes = clipInfo.has_explicit_time_signature_changes;
        if (clipInfo.has_explicit_tempo_changes)
            result.masterTrackClip.tempo_changes = clipInfo.tempo_changes;
        if (clipInfo.has_explicit_time_signature_changes)
            result.masterTrackClip.time_signature_changes = clipInfo.time_signature_changes;

        // The musical clip's own tempo/time-signature curve belongs to the master track now --
        // keep only a flat reference tempo so this clip never looks like a tempo/time-signature
        // authority (see clipHasMeaningfulTempoMap/clipHasMeaningfulTimeSignatureMap).
        stripToFlatTempo(result.musicalClip.tempo_changes, result.musicalClip.time_signature_changes, result.musicalClip.tempo);

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

    void MidiClipReader::rescaleTicks(
        std::vector<uint64_t>& tickTimestamps,
        std::vector<MidiTempoChange>& tempoChanges,
        std::vector<MidiTimeSignatureChange>& timeSignatureChanges,
        uint32_t fromResolution,
        uint32_t toResolution
    ) {
        if (fromResolution == toResolution || fromResolution == 0)
            return;
        const double ratio = static_cast<double>(toResolution) / static_cast<double>(fromResolution);
        for (auto& t : tickTimestamps)
            t = static_cast<uint64_t>(std::llround(static_cast<double>(t) * ratio));
        for (auto& change : tempoChanges)
            change.tickPosition = static_cast<uint64_t>(std::llround(static_cast<double>(change.tickPosition) * ratio));
        for (auto& change : timeSignatureChanges)
            change.tickPosition = static_cast<uint64_t>(std::llround(static_cast<double>(change.tickPosition) * ratio));
    }

    void MidiClipReader::stripToFlatTempo(
        std::vector<MidiTempoChange>& tempoChanges,
        std::vector<MidiTimeSignatureChange>& timeSignatureChanges,
        double flatTempo
    ) {
        tempoChanges.assign(1, MidiTempoChange{0, flatTempo});
        timeSignatureChanges.assign(1, MidiTimeSignatureChange{0, 4, 4});
    }

    std::vector<MidiTempoChange> MidiClipReader::applyAuthoritativeTempoMapToMusicalClips(
        const std::shared_ptr<TimelineTrack>& masterTrack,
        const std::vector<std::shared_ptr<TimelineTrack>>& tracks
    ) {
        auto* authoritative = findAuthoritativeTimelineMetaSource(masterTrack);
        for (const auto& track : tracks) {
            if (!track)
                continue;
            auto clips = track->clipManager().getAllClips();
            for (const auto& clip : clips) {
                if (clip.clipType != ClipType::Midi)
                    continue;
                auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
                auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                if (!midiNode)
                    continue;
                if (authoritative)
                    midiNode->setPlaybackTempoMap(authoritative->tempoChanges());
                else
                    midiNode->clearPlaybackTempoMap();

                // ClipData.durationSamples was cached from sourceNode->totalLength() at
                // add-time, using whichever tempo map the clip had *then* -- which may have been
                // this clip's own (possibly flat/stripped, e.g. after a save/reload cycle) tempo
                // before the authoritative map above corrected it. Refresh it now so
                // content-bounds/render-length calculations match the corrected schedule instead
                // of silently truncating or extending playback.
                const int64_t correctedDuration = midiNode->totalLength();
                if (correctedDuration != clip.durationSamples)
                    track->clipManager().resizeClip(clip.clipId, correctedDuration);
            }
        }

        if (authoritative)
            return authoritative->tempoChanges();
        return {};
    }

} // namespace uapmd
