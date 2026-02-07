#include <uapmd-data/priv/project/SmfConverter.hpp>
#include <umppi/umppi.hpp>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <format>

namespace uapmd {

    SmfConverter::ConvertResult SmfConverter::convertToUmp(const std::filesystem::path& smfFile) {
        ConvertResult result;

        try {
            // Read SMF file using umppi
            umppi::Midi1Music music = umppi::readMidi1File(smfFile.string());

            // Store tick resolution
            result.tickResolution = music.deltaTimeSpec;

            // Check if division is in ticks per quarter note (bit 15 = 0)
            if (result.tickResolution & 0x8000) {
                result.error = "SMPTE time division not supported";
                return result;
            }

            // Merge tracks for Format 0 (combine all tracks into one)
            std::vector<std::pair<int, umppi::Midi1Event>> allEvents;
            int cumulativeTime = 0;

            for (const auto& track : music.tracks) {
                cumulativeTime = 0;
                for (const auto& event : track.events) {
                    cumulativeTime += event.deltaTime;
                    allEvents.emplace_back(cumulativeTime, event);
                }
            }

            // Sort events by cumulative time
            std::sort(allEvents.begin(), allEvents.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            // Convert merged events to MIDI 1.0 byte stream
            std::vector<uint8_t> midi1Bytes;
            int previousTime = 0;

            for (const auto& [absTime, event] : allEvents) {
                const auto& msg = event.message;
                if (!msg) continue;

                // Calculate delta time
                int deltaTime = absTime - previousTime;
                previousTime = absTime;

                // Encode variable-length delta time
                auto deltaBytes = umppi::Midi1Event::encode7BitLength(deltaTime);
                midi1Bytes.insert(midi1Bytes.end(), deltaBytes.rbegin(), deltaBytes.rend());

                // Add message bytes
                int value = msg->getValue();
                uint8_t status = msg->getStatusByte();

                // Check for tempo meta-event
                if (status == umppi::Midi1Status::META && msg->getMetaType() == 0x51) {
                    // Tempo meta-event
                    if (auto* compound = dynamic_cast<umppi::Midi1CompoundMessage*>(msg.get())) {
                        uint32_t microsecondsPerQuarter =
                            (compound->getExtraData()[0] << 16) |
                            (compound->getExtraData()[1] << 8) |
                            compound->getExtraData()[2];
                        result.detectedTempo = 60000000.0 / microsecondsPerQuarter;
                    }
                }

                // Write status byte
                midi1Bytes.push_back(status);

                // Write data bytes
                if (status == umppi::Midi1Status::META ||
                    status == umppi::Midi1Status::SYSEX ||
                    status == umppi::Midi1Status::SYSEX_END) {
                    // Meta event or SysEx
                    if (auto* compound = dynamic_cast<umppi::Midi1CompoundMessage*>(msg.get())) {
                        if (status == umppi::Midi1Status::META) {
                            midi1Bytes.push_back(msg->getMsb());  // Meta type
                        }
                        const auto& extraData = compound->getExtraData();
                        auto lenBytes = umppi::Midi1Event::encode7BitLength(extraData.size());
                        midi1Bytes.insert(midi1Bytes.end(), lenBytes.rbegin(), lenBytes.rend());
                        midi1Bytes.insert(midi1Bytes.end(), extraData.begin(), extraData.end());
                    }
                } else {
                    // Regular MIDI message
                    uint8_t dataSize = umppi::Midi1Message::fixedDataSize(status);
                    if (dataSize >= 1) midi1Bytes.push_back(msg->getMsb());
                    if (dataSize >= 2) midi1Bytes.push_back(msg->getLsb());
                }
            }

            // Convert MIDI 1.0 bytes to UMP
            umppi::Midi1ToUmpTranslatorContext context(midi1Bytes, 0, false,
                static_cast<int>(umppi::MidiTransportProtocol::MIDI1), false, true);

            int translateResult = umppi::UmpTranslator::translateMidi1BytesToUmp(context);

            if (translateResult != umppi::UmpTranslationResult::OK) {
                result.error = std::format("UMP translation failed with code: {}", translateResult);
                return result;
            }

            // Convert umppi::Ump to uapmd_ump_t and store tick timestamps
            result.umpEvents.reserve(context.output.size() * 4);  // Max 4 words per UMP
            result.umpEventTicksStamps.reserve(context.output.size() * 4);

            // Build tick timestamp map from allEvents
            size_t umpIndex = 0;
            for (size_t eventIdx = 0; eventIdx < allEvents.size() && umpIndex < context.output.size(); ++eventIdx) {
                const auto& [absTime, event] = allEvents[eventIdx];
                const auto& ump = context.output[umpIndex];

                // Each umppi::Ump can be 32, 64, or 128 bits
                result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int1));
                result.umpEventTicksStamps.push_back(static_cast<uint64_t>(absTime));

                if (ump.getSizeInBytes() >= 8) {
                    result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int2));
                    result.umpEventTicksStamps.push_back(static_cast<uint64_t>(absTime));
                }
                if (ump.getSizeInBytes() >= 12) {
                    result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int3));
                    result.umpEventTicksStamps.push_back(static_cast<uint64_t>(absTime));
                }
                if (ump.getSizeInBytes() >= 16) {
                    result.umpEvents.push_back(static_cast<uapmd_ump_t>(ump.int4));
                    result.umpEventTicksStamps.push_back(static_cast<uint64_t>(absTime));
                }

                umpIndex++;
            }

            result.success = true;

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
