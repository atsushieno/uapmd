#include <uapmd-data/priv/project/SmfConverter.hpp>
#include <umppi/umppi.hpp>
#include <fstream>
#include <format>

namespace uapmd {

namespace {

SmfConverter::ConvertResult convertEventsToUmp(const std::vector<umppi::Midi1Event>& events,
                                               uint32_t tickResolution) {
    SmfConverter::ConvertResult result;
    result.tickResolution = tickResolution;

    if (events.empty()) {
        result.success = true;
        return result;
    }

    std::vector<uint8_t> midi1Bytes;
    midi1Bytes.reserve(events.size() * 4);
    std::vector<uint64_t> eventTicks;
    eventTicks.reserve(events.size());

    int absoluteTicks = 0;
    int previousMessageTick = 0;
    bool hasEmittedEvent = false;

    for (const auto& event : events) {
        absoluteTicks += event.deltaTime;
        const auto& msg = event.message;
        if (!msg) {
            continue;
        }

        int deltaTime = hasEmittedEvent ? absoluteTicks - previousMessageTick : absoluteTicks;
        previousMessageTick = absoluteTicks;
        hasEmittedEvent = true;

        auto deltaBytes = umppi::Midi1Event::encode7BitLength(deltaTime);
        midi1Bytes.insert(midi1Bytes.end(), deltaBytes.begin(), deltaBytes.end());

        uint8_t status = msg->getStatusByte();

        if (status == umppi::Midi1Status::META && msg->getMetaType() == 0x51) {
            if (auto* compound = dynamic_cast<umppi::Midi1CompoundMessage*>(msg.get())) {
                const auto& extraData = compound->getExtraData();
                if (extraData.size() >= 3) {
                    uint32_t microsecondsPerQuarter =
                        (static_cast<uint32_t>(extraData[0]) << 16) |
                        (static_cast<uint32_t>(extraData[1]) << 8) |
                        static_cast<uint32_t>(extraData[2]);
                    result.detectedTempo = 60000000.0 / microsecondsPerQuarter;
                }
            }
        }

        midi1Bytes.push_back(status);

        if (status == umppi::Midi1Status::META ||
            status == umppi::Midi1Status::SYSEX ||
            status == umppi::Midi1Status::SYSEX_END) {
            if (auto* compound = dynamic_cast<umppi::Midi1CompoundMessage*>(msg.get())) {
                if (status == umppi::Midi1Status::META) {
                    midi1Bytes.push_back(msg->getMsb());
                }
                const auto& extraData = compound->getExtraData();
                auto lenBytes = umppi::Midi1Event::encode7BitLength(static_cast<int>(extraData.size()));
                midi1Bytes.insert(midi1Bytes.end(), lenBytes.rbegin(), lenBytes.rend());
                midi1Bytes.insert(midi1Bytes.end(), extraData.begin(), extraData.end());
            }
        } else {
            uint8_t dataSize = umppi::Midi1Message::fixedDataSize(status);
            if (dataSize >= 1) midi1Bytes.push_back(msg->getMsb());
            if (dataSize >= 2) midi1Bytes.push_back(msg->getLsb());
        }

        eventTicks.push_back(static_cast<uint64_t>(absoluteTicks));
    }

    if (midi1Bytes.empty()) {
        result.success = true;
        return result;
    }

    umppi::Midi1ToUmpTranslatorContext context(
        midi1Bytes, 0, false,
        static_cast<int>(umppi::MidiTransportProtocol::MIDI1), false, true);

    int translateResult = umppi::UmpTranslator::translateMidi1BytesToUmp(context);

    if (translateResult != umppi::UmpTranslationResult::OK) {
        result.error = std::format("UMP translation failed with code: {}", translateResult);
        return result;
    }

    result.umpEvents.reserve(context.output.size() * 4);
    result.umpEventTicksStamps.reserve(context.output.size() * 4);

    size_t umpIndex = 0;
    for (size_t eventIdx = 0;
         eventIdx < eventTicks.size() && umpIndex < context.output.size();
         ++eventIdx, ++umpIndex) {
        uint64_t absTime = eventTicks[eventIdx];
        const auto& ump = context.output[umpIndex];

        result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int1));
        result.umpEventTicksStamps.push_back(absTime);

        if (ump.getSizeInBytes() >= 8) {
            result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int2));
            result.umpEventTicksStamps.push_back(absTime);
        }
        if (ump.getSizeInBytes() >= 12) {
            result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int3));
            result.umpEventTicksStamps.push_back(absTime);
        }
        if (ump.getSizeInBytes() >= 16) {
            result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int4));
            result.umpEventTicksStamps.push_back(absTime);
        }
    }

    result.success = true;
    return result;
}

} // namespace

    SmfConverter::ConvertResult SmfConverter::convertToUmp(const std::filesystem::path& smfFile) {
        ConvertResult result;

        try {
            umppi::Midi1Music music = umppi::readMidi1File(smfFile.string());

            result.tickResolution = music.deltaTimeSpec;

            if (result.tickResolution & 0x8000) {
                result.error = "SMPTE time division not supported";
                return result;
            }

            const umppi::Midi1Music* musicToConvert = &music;
            umppi::Midi1Music mergedMusic;

            if (music.format != 0) {
                mergedMusic = music.mergeTracks();
                musicToConvert = &mergedMusic;
            }

            if (musicToConvert->tracks.empty()) {
                result.error = "SMF file contains no tracks";
                return result;
            }

            result = convertEventsToUmp(musicToConvert->tracks.front().events, result.tickResolution);

        } catch (const std::exception& e) {
            result.error = std::string("Exception during SMF conversion: ") + e.what();
            result.success = false;
        }

        return result;
    }

    double SmfConverter::extractTempoFromSmf(const std::vector<uint8_t>& smfData) {
        // Default tempo: 120 BPM (500000 microseconds per quarter note)
        double bpm = 120.0;

        // Search for tempo meta-event (0xFF 0x51 0x03 tt tt tt)
        for (size_t i = 0; i < smfData.size() - 5; i++) {
            if (smfData[i] == 0xFF && smfData[i+1] == 0x51 && smfData[i+2] == 0x03) {
                // Extract microseconds per quarter note (24-bit big-endian)
                uint32_t microsecondsPerQuarter =
                    (smfData[i+3] << 16) | (smfData[i+4] << 8) | smfData[i+5];

                // Convert to BPM: BPM = 60,000,000 / microsecondsPerQuarter
                bpm = 60000000.0 / microsecondsPerQuarter;
                break;
            }
        }

        return bpm;
    }

    SmfConverter::ConvertResult SmfConverter::convertTrackToUmp(const std::filesystem::path& smfFile, size_t trackIndex) {
        ConvertResult result;

        try {
            umppi::Midi1Music music = umppi::readMidi1File(smfFile.string());

            result.tickResolution = music.deltaTimeSpec;

            if (result.tickResolution & 0x8000) {
                result.error = "SMPTE time division not supported";
                return result;
            }

            if (trackIndex >= music.tracks.size()) {
                result.error = std::format("Track index {} out of range (file has {} tracks)", trackIndex, music.tracks.size());
                return result;
            }

            result = convertEventsToUmp(music.tracks[trackIndex].events, result.tickResolution);

        } catch (const std::exception& e) {
            result.error = std::string("Exception during SMF track conversion: ") + e.what();
            result.success = false;
        }

        return result;
    }

    bool SmfConverter::isValidSmfFile(const std::filesystem::path& file) {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open())
            return false;

        char header[4];
        f.read(header, 4);

        return (header[0] == 'M' && header[1] == 'T' &&
                header[2] == 'h' && header[3] == 'd');
    }

} // namespace uapmd
