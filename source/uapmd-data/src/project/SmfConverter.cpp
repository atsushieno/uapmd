#include <fstream>
#include <format>
#include <algorithm>
#include <umppi/umppi.hpp>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

namespace {

constexpr uint8_t kTempoMetaType = 0x51;
constexpr uint8_t kTimeSignatureMetaType = 0x58;

double microsecondsPerQuarterToBpm(uint32_t value) {
    if (value == 0)
        return 120.0;
    return 60000000.0 / static_cast<double>(value);
}

uint8_t pow2ToDenominator(uint8_t exponent) {
    if (exponent > 7)
        exponent = 7;
    return static_cast<uint8_t>(1u << exponent);
}

void collectMetaEvent(const std::shared_ptr<umppi::Midi1Message>& msg,
                      uint64_t absoluteTicks,
                      SmfConverter::ConvertResult& result,
                      bool& tempoDetected,
                      bool& initialTempoSet,
                      bool& timeSignatureDetected) {
    if (!msg)
        return;

    if (msg->getStatusByte() != umppi::Midi1Status::META)
        return;

    if (msg->getMetaType() == kTempoMetaType) {
        if (auto* compound = dynamic_cast<umppi::Midi1CompoundMessage*>(msg.get())) {
            const auto& extraData = compound->getExtraData();
            if (extraData.size() >= 3) {
                uint32_t microsecondsPerQuarter =
                    (static_cast<uint32_t>(extraData[0]) << 16) |
                    (static_cast<uint32_t>(extraData[1]) << 8) |
                    static_cast<uint32_t>(extraData[2]);
                double bpm = microsecondsPerQuarterToBpm(microsecondsPerQuarter);

                result.tempoChanges.push_back(MidiTempoChange{absoluteTicks, bpm});
                tempoDetected = true;
                if (!initialTempoSet) {
                    result.detectedTempo = bpm;
                    initialTempoSet = true;
                }
            }
        }
    } else if (msg->getMetaType() == kTimeSignatureMetaType) {
        if (auto* compound = dynamic_cast<umppi::Midi1CompoundMessage*>(msg.get())) {
            const auto& extraData = compound->getExtraData();
            if (extraData.size() >= 2) {
                uint8_t numerator = extraData[0];
                uint8_t denominator = pow2ToDenominator(extraData[1]);
                uint8_t clocksPerClick = extraData.size() >= 3 ? extraData[2] : 24;
                uint8_t thirtySecondsPerQuarter = extraData.size() >= 4 ? extraData[3] : 8;
                result.timeSignatureChanges.push_back(
                    MidiTimeSignatureChange{
                        absoluteTicks,
                        numerator,
                        denominator,
                        clocksPerClick,
                        thirtySecondsPerQuarter
                    });
                timeSignatureDetected = true;
            }
        }
    }
}

void ensureDefaultMetaEvents(SmfConverter::ConvertResult& result,
                             bool tempoDetected,
                             bool timeSignatureDetected) {
    result.hasExplicitTempoChanges = tempoDetected;
    result.hasExplicitTimeSignatureChanges = timeSignatureDetected;
    if (!tempoDetected) {
        result.tempoChanges.push_back(MidiTempoChange{0, result.detectedTempo});
    }
    if (!timeSignatureDetected) {
        result.timeSignatureChanges.push_back(MidiTimeSignatureChange{0, 4, 4});
    }
}

void collectMetaEventsFromEvents(const std::vector<umppi::Midi1Event>& events,
                                 SmfConverter::ConvertResult& result) {
    result.tempoChanges.clear();
    result.timeSignatureChanges.clear();

    bool tempoDetected = false;
    bool timeSignatureDetected = false;
    bool initialTempoSet = false;
    uint64_t absoluteTicks = 0;

    for (const auto& event : events) {
        absoluteTicks += event.deltaTime;
        collectMetaEvent(event.message, absoluteTicks, result,
                         tempoDetected, initialTempoSet, timeSignatureDetected);
    }

    ensureDefaultMetaEvents(result, tempoDetected, timeSignatureDetected);
}

void collectMetaEventsFromMusic(const umppi::Midi1Music& music,
                                SmfConverter::ConvertResult& result) {
    const umppi::Midi1Music* source = &music;
    umppi::Midi1Music mergedMusic;
    if (music.format != 0) {
        mergedMusic = music.mergeTracks();
        source = &mergedMusic;
    }

    if (source->tracks.empty())
        return;

    collectMetaEventsFromEvents(source->tracks.front().events, result);
}

SmfConverter::ConvertResult convertEventsToUmp(const std::vector<umppi::Midi1Event>& events,
                                               uint32_t tickResolution,
                                               bool captureMetaEvents = true) {
    SmfConverter::ConvertResult result;
    result.tickResolution = tickResolution;

    if (events.empty()) {
        result.success = true;
        if (captureMetaEvents) {
            result.tempoChanges.push_back(MidiTempoChange{0, result.detectedTempo});
            result.timeSignatureChanges.push_back(MidiTimeSignatureChange{0, 4, 4});
        }
        return result;
    }

    // MIDI 2.0 channel voice messages are 64-bit (2 words each)
    result.umpEvents.reserve(events.size() * 2);
    result.umpEventTicksStamps.reserve(events.size() * 2);

    uint64_t absoluteTicks = 0;
    bool tempoDetected = false;
    bool timeSignatureDetected = false;
    bool initialTempoSet = false;
    constexpr uint8_t group = 0;

    for (const auto& event : events) {
        absoluteTicks += event.deltaTime;
        const auto& msg = event.message;
        if (!msg)
            continue;

        uint8_t status = msg->getStatusByte();

        if (captureMetaEvents)
            collectMetaEvent(msg, absoluteTicks, result,
                             tempoDetected, initialTempoSet, timeSignatureDetected);

        if (status == umppi::Midi1Status::META ||
            status == umppi::Midi1Status::SYSEX ||
            status == umppi::Midi1Status::SYSEX_END)
            continue;

        uint8_t statusCode = msg->getStatusCode();
        uint8_t channel = msg->getChannel();
        uint8_t msb = msg->getMsb();
        uint8_t lsb = msg->getLsb();

        // Build a MIDI 2.0 64-bit UMP.
        // Scaling conventions (matching UmpInputDispatcher reverse conversions):
        //   7-bit → 16-bit velocity : val << 9
        //   7-bit → 32-bit data     : val << 25
        //  14-bit → 32-bit pitchbend: val << 18
        uint64_t ump2 = 0;

        switch (statusCode) {
            case umppi::MidiChannelStatus::NOTE_OFF:
                ump2 = umppi::UmpFactory::midi2NoteOff(group, channel, msb, 0,
                    static_cast<uint16_t>(lsb) << 9, 0);
                break;
            case umppi::MidiChannelStatus::NOTE_ON:
                // MIDI 1.0 convention: note-on velocity=0 means note-off
                if (lsb == 0)
                    ump2 = umppi::UmpFactory::midi2NoteOff(group, channel, msb, 0, 0, 0);
                else
                    ump2 = umppi::UmpFactory::midi2NoteOn(group, channel, msb, 0,
                        static_cast<uint16_t>(lsb) << 9, 0);
                break;
            case umppi::MidiChannelStatus::PAF:
                ump2 = umppi::UmpFactory::midi2PAf(group, channel, msb,
                    static_cast<uint32_t>(lsb) << 25);
                break;
            case umppi::MidiChannelStatus::CC:
                ump2 = umppi::UmpFactory::midi2CC(group, channel, msb,
                    static_cast<uint32_t>(lsb) << 25);
                break;
            case umppi::MidiChannelStatus::PROGRAM:
                ump2 = umppi::UmpFactory::midi2Program(group, channel, 0, msb, 0, 0);
                break;
            case umppi::MidiChannelStatus::CAF:
                ump2 = umppi::UmpFactory::midi2CAf(group, channel,
                    static_cast<uint32_t>(msb) << 25);
                break;
            case umppi::MidiChannelStatus::PITCH_BEND: {
                uint32_t pitchData14 = static_cast<uint32_t>(msb) | (static_cast<uint32_t>(lsb) << 7);
                ump2 = umppi::UmpFactory::midi2PitchBendDirect(group, channel, pitchData14 << 18);
                break;
            }
            default:
                if ((status & 0xF0) == 0xF0) {
                    // System real-time stays as a 32-bit MIDI 1.0 system UMP (type 1)
                    uint32_t sysRt = umppi::UmpFactory::systemMessage(group, status, msb, lsb);
                    result.umpEvents.push_back(static_cast<uapmd_ump_t>(sysRt));
                    result.umpEventTicksStamps.push_back(absoluteTicks);
                }
                continue;
        }

        if (ump2 != 0) {
            // Factory packs (word0 << 32) | word1; emit both words with same tick
            result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump2 >> 32));
            result.umpEventTicksStamps.push_back(absoluteTicks);
            result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump2 & 0xFFFFFFFFU));
            result.umpEventTicksStamps.push_back(absoluteTicks);
        }
    }

    if (captureMetaEvents)
        ensureDefaultMetaEvents(result, tempoDetected, timeSignatureDetected);

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

            result = convertEventsToUmp(musicToConvert->tracks.front().events, result.tickResolution, true);

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

    SmfConverter::ConvertResult SmfConverter::convertTrackToUmp(const umppi::Midi1Music& music, size_t trackIndex) {
        ConvertResult result;

        try {
            result.tickResolution = music.deltaTimeSpec;

            if (result.tickResolution & 0x8000) {
                result.error = "SMPTE time division not supported";
                return result;
            }

            if (trackIndex >= music.tracks.size()) {
                result.error = std::format("Track index {} out of range (file has {} tracks)", trackIndex, music.tracks.size());
                return result;
            }

            result = convertEventsToUmp(music.tracks[trackIndex].events, result.tickResolution, false);

            // Tempo/time-signature events may reside on any track, so collect them
            collectMetaEventsFromMusic(music, result);

        } catch (const std::exception& e) {
            result.error = std::string("Exception during SMF track conversion: ") + e.what();
            result.success = false;
        }

        return result;
    }

    SmfConverter::ConvertResult SmfConverter::convertTrackToUmp(const std::filesystem::path& smfFile, size_t trackIndex) {
        ConvertResult result;

        try {
            umppi::Midi1Music music = umppi::readMidi1File(smfFile.string());
            return convertTrackToUmp(music, trackIndex);

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
