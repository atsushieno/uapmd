#include "ProjectSerialization.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <umppi/umppi.hpp>

namespace uapmd::sequencer_detail {

    namespace {
        constexpr uint8_t kTempoGroup = 0;
        constexpr uint8_t kTempoChannel = 0;

        struct ScheduledUmp {
            uint64_t tick{0};
            int priority{0};
            umppi::Ump message{};
        };

        struct GatheredClipEvents {
            std::vector<ScheduledUmp> events;
            uint64_t endTick{0};
        };

        bool isSafeRelativeExtensionPath(const std::filesystem::path& path) {
            if (path.empty() || path.is_absolute())
                return false;
            for (const auto& part : path)
                if (part == "..")
                    return false;
            return true;
        }

        std::string escapeExtensionPathComponent(std::string_view value) {
            std::string result;
            for (auto ch : value) {
                auto c = static_cast<unsigned char>(ch);
                if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
                    result.push_back(static_cast<char>(c));
                } else {
                    result += std::format("%{:02X}", c);
                }
            }
            if (result.empty())
                return "extension";
            return result;
        }

        std::filesystem::path extensionDataRoot(
            const std::filesystem::path& projectDir,
            std::string_view extensionId) {
            return projectDir / "extensions" / escapeExtensionPathComponent(extensionId);
        }

        uint32_t bpmToTenNanoseconds(double bpm) {
            double clampedBpm = std::clamp(bpm, 0.0001, 960.0);
            double value = 6000000000.0 / clampedBpm;
            value = std::clamp(value, 1.0, static_cast<double>(std::numeric_limits<uint32_t>::max()));
            return static_cast<uint32_t>(value);
        }

        GatheredClipEvents gatherMidiClipEvents(const MidiClipSourceNode& node, bool includeTimelineMeta) {
            GatheredClipEvents result;
            const auto& eventWords = node.umpEvents();
            const auto& eventTicks = node.eventTimestampsTicks();
            const auto& tempoChanges = node.tempoChanges();
            const auto& timeSigChanges = node.timeSignatureChanges();

            auto priorityFor = [](const umppi::Ump& message) {
                if (message.isTempo())
                    return 0;
                if (message.isTimeSignature())
                    return 1;
                return 2;
            };

            std::unordered_set<uint64_t> tempoTicks;
            std::unordered_set<uint64_t> timeSigTicks;

            size_t wordIndex = 0;
            auto readNextUmp = [&](umppi::Ump& message, uint64_t& eventTick) -> bool {
                if (wordIndex >= eventWords.size())
                    return false;
                const uint32_t word = eventWords[wordIndex];
                const auto messageType = static_cast<uint8_t>((word >> 28) & 0xF);
                const int wordCount = umppi::umpSizeInInts(messageType);
                if (wordCount <= 0)
                    return false;
                if (wordIndex + static_cast<size_t>(wordCount) > eventWords.size())
                    return false;

                switch (wordCount) {
                    case 1:
                        message = umppi::Ump(word);
                        break;
                    case 2:
                        message = umppi::Ump(word, eventWords[wordIndex + 1]);
                        break;
                    case 4:
                        message = umppi::Ump(
                            word,
                            eventWords[wordIndex + 1],
                            eventWords[wordIndex + 2],
                            eventWords[wordIndex + 3]);
                        break;
                    default:
                        return false;
                }

                eventTick = (!eventTicks.empty() && wordIndex < eventTicks.size())
                    ? eventTicks[wordIndex]
                    : 0;
                wordIndex += static_cast<size_t>(wordCount);
                return true;
            };

            bool reachedEnd = false;
            while (wordIndex < eventWords.size()) {
                umppi::Ump message;
                uint64_t absoluteTick = 0;
                if (!readNextUmp(message, absoluteTick))
                    break;

                if (message.isDeltaClockstamp() || message.isDCTPQ() || message.isStartOfClip())
                    continue;

                if (message.isEndOfClip()) {
                    result.endTick = std::max(result.endTick, absoluteTick);
                    reachedEnd = true;
                    break;
                }

                ScheduledUmp entry;
                entry.tick = absoluteTick;
                entry.priority = priorityFor(message);
                entry.message = message;
                result.events.push_back(entry);
                if (entry.tick > result.endTick)
                    result.endTick = entry.tick;

                if (message.isTempo())
                    tempoTicks.insert(entry.tick);
                else if (message.isTimeSignature())
                    timeSigTicks.insert(entry.tick);
            }

            if (!reachedEnd && !eventTicks.empty())
                result.endTick = std::max(result.endTick, eventTicks.back());

            if (includeTimelineMeta) {
                for (const auto& tempo : tempoChanges) {
                    if (tempoTicks.contains(tempo.tickPosition))
                        continue;

                    ScheduledUmp entry;
                    entry.tick = tempo.tickPosition;
                    entry.priority = 0;
                    const double bpm = tempo.bpm > 0.0 ? tempo.bpm : 120.0;
                    entry.message = umppi::UmpFactory::tempo(
                        kTempoGroup,
                        kTempoChannel,
                        bpmToTenNanoseconds(bpm));
                    result.events.push_back(entry);
                    tempoTicks.insert(entry.tick);
                    if (entry.tick > result.endTick)
                        result.endTick = entry.tick;
                }

                for (const auto& sig : timeSigChanges) {
                    if (timeSigTicks.contains(sig.tickPosition))
                        continue;

                    ScheduledUmp entry;
                    entry.tick = sig.tickPosition;
                    entry.priority = 1;
                    entry.message = umppi::UmpFactory::timeSignatureDirect(
                        kTempoGroup,
                        kTempoChannel,
                        sig.numerator,
                        sig.denominator,
                        0);
                    result.events.push_back(entry);
                    timeSigTicks.insert(entry.tick);
                    if (entry.tick > result.endTick)
                        result.endTick = entry.tick;
                }
            }

            std::stable_sort(result.events.begin(), result.events.end(), [](const ScheduledUmp& a, const ScheduledUmp& b) {
                if (a.tick != b.tick)
                    return a.tick < b.tick;
                return a.priority < b.priority;
            });

            if (!result.events.empty() && result.events.back().tick > result.endTick)
                result.endTick = result.events.back().tick;

            return result;
        }

        std::vector<umppi::Ump> buildSmf2ClipFromMidiNode(const MidiClipSourceNode& node, bool includeTimelineMeta) {
            std::vector<umppi::Ump> clip;
            clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
            clip.emplace_back(umppi::Ump(umppi::UmpFactory::dctpq(node.tickResolution())));
            clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
            clip.push_back(umppi::UmpFactory::startOfClip());

            auto gathered = gatherMidiClipEvents(node, includeTimelineMeta);
            uint64_t previousTick = 0;
            for (const auto& entry : gathered.events) {
                uint64_t delta = entry.tick >= previousTick ? entry.tick - previousTick : 0;
                clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(static_cast<uint32_t>(delta))));
                clip.push_back(entry.message);
                previousTick = entry.tick;
            }

            uint64_t tailDelta = gathered.endTick > previousTick ? gathered.endTick - previousTick : 0;
            clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(static_cast<uint32_t>(tailDelta))));
            clip.push_back(umppi::UmpFactory::endOfClip());

            return clip;
        }
    } // namespace

    std::filesystem::path makeRelativePath(
        const std::filesystem::path& baseDir,
        const std::filesystem::path& target) {
        if (baseDir.empty() || target.empty())
            return target;

        std::error_code ec;
        auto rel = std::filesystem::relative(target, baseDir, ec);
        if (ec)
            return target;

        for (const auto& part : rel)
            if (part == "..")
                return target;
        return rel;
    }

    std::vector<ClipData> sortedTrackClips(TimelineTrack& timelineTrack) {
        auto clips = timelineTrack.clipManager().getAllClips();
        std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
            return a.clipId < b.clipId;
        });
        return clips;
    }

    std::string writePluginStateBlob(
        const std::filesystem::path& projectDir,
        const std::filesystem::path& pluginStateDir,
        const std::string& scopeLabel,
        size_t pluginOrder,
        int32_t instanceId,
        const std::vector<uint8_t>& stateData,
        std::string& error) {
        std::error_code createDirEc;
        std::filesystem::create_directories(pluginStateDir, createDirEc);
        if (createDirEc) {
            error = std::format("Failed to create plugin state directory: {} ({})",
                                pluginStateDir.string(),
                                createDirEc.message());
            return {};
        }

        auto filename = std::format("{}_plugin{}_instance{}.state",
                                    scopeLabel,
                                    pluginOrder,
                                    instanceId);
        auto targetPath = pluginStateDir / filename;

        try {
            std::ofstream out(targetPath, std::ios::binary);
            if (!out)
                throw std::runtime_error("Failed to open state file for writing");
            out.write(reinterpret_cast<const char*>(stateData.data()),
                      static_cast<std::streamsize>(stateData.size()));
        } catch (const std::exception& ex) {
            error = std::format("Failed to write plugin state to {}: {}",
                                targetPath.string(),
                                ex.what());
            return {};
        }

        auto recordedPath = targetPath;
        if (!projectDir.empty())
            recordedPath = makeRelativePath(projectDir, recordedPath);
        return recordedPath.generic_string();
    }

    bool serializeProjectClip(
        TimelineTrack& timelineTrack,
        const ClipData& clip,
        UapmdProjectTrackData& projectTrack,
        std::unordered_map<std::string, UapmdProjectClipData*>& serializedClipLookup,
        const std::filesystem::path& clipDir,
        const std::filesystem::path& projectDir,
        const std::string& midiExportNamePrefix,
        const std::string& audioCopyNamePrefix,
        const std::string& clipContextLabel,
        bool includeTimelineMeta,
        size_t& midiExportCounter,
        std::string& error) {
        auto projectClip = UapmdProjectClipData::create();
        projectClip->clipType(clip.clipType == ClipType::Midi ? "midi" : "audio");
        projectClip->tickResolution(clip.tickResolution);
        projectClip->markers(clip.markers);
        projectClip->audioWarps(clip.audioWarps);

        std::filesystem::path clipPath = clip.filepath;
        if (clip.clipType == ClipType::Midi) {
            bool needsExport = clip.needsFileSave || clipPath.empty() || !std::filesystem::exists(clipPath);
            if (needsExport) {
                std::filesystem::create_directories(clipDir);
                auto exportName = std::format("{}{}_{}.midi2",
                                              midiExportNamePrefix,
                                              clip.clipId,
                                              midiExportCounter++);
                auto exportPath = clipDir / exportName;

                auto sourceNode = timelineTrack.getSourceNode(clip.sourceNodeInstanceId);
                auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                if (!midiNode) {
                    error = std::format("{} is missing MIDI data", clipContextLabel);
                    return false;
                }

                std::string writeError;
                auto clipUmps = buildSmf2ClipFromMidiNode(*midiNode, includeTimelineMeta);
                if (!Smf2ClipReaderWriter::write(exportPath, clipUmps, &writeError)) {
                    error = std::move(writeError);
                    return false;
                }
                clipPath = exportPath;
            } else {
                clipPath = std::filesystem::absolute(clipPath);
            }
        } else {
            if (clip.needsFileSave) {
                if (clipPath.empty()) {
                    error = std::format("{} has no source audio to save", clipContextLabel);
                    return false;
                }

                auto sourcePath = std::filesystem::absolute(clipPath);
                if (!std::filesystem::exists(sourcePath)) {
                    error = std::format("{} is missing its audio file", clipContextLabel);
                    return false;
                }

                std::error_code dirEc;
                std::filesystem::create_directories(clipDir, dirEc);
                if (dirEc) {
                    error = std::format("Failed to create clip directory: {}", dirEc.message());
                    return false;
                }

                auto originalName = sourcePath.filename().string();
                if (originalName.empty())
                    originalName = std::format("clip{}_audio.wav", clip.clipId);
                auto destPath = clipDir / std::format("{}{}", audioCopyNamePrefix, originalName);

                std::error_code copyEc;
                std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::overwrite_existing, copyEc);
                if (copyEc) {
                    error = std::format("Failed to store audio clip {}: {}", clip.clipId, copyEc.message());
                    return false;
                }

                clipPath = destPath;
            } else if (!clipPath.empty()) {
                clipPath = std::filesystem::absolute(clipPath);
            }
        }

        if (!clipPath.empty() && !projectDir.empty())
            clipPath = makeRelativePath(projectDir, clipPath);

        projectClip->file(clipPath);
        serializedClipLookup[clip.referenceId] = projectClip.get();
        projectTrack.clips().push_back(std::move(projectClip));
        return true;
    }

    FilesystemProjectSerializationWriteContext::FilesystemProjectSerializationWriteContext(
        std::filesystem::path projectFile,
        std::filesystem::path projectDir)
        : project_file_(std::move(projectFile))
        , project_dir_(std::move(projectDir)) {
    }

    std::filesystem::path FilesystemProjectSerializationWriteContext::projectFile() const {
        return project_file_;
    }

    std::filesystem::path FilesystemProjectSerializationWriteContext::projectDirectory() const {
        return project_dir_;
    }

    bool FilesystemProjectSerializationWriteContext::writeExtensionFile(
        std::string_view extensionId,
        const std::filesystem::path& relativePath,
        const std::vector<uint8_t>& data,
        std::string& error) {
        if (!isSafeRelativeExtensionPath(relativePath)) {
            error = "Extension attempted to write an unsafe project-relative path.";
            return false;
        }

        auto root = extensionDataRoot(project_dir_, extensionId);
        auto target = (root / relativePath).lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            error = std::format("Failed to create extension data directory {}: {}",
                                target.parent_path().string(),
                                ec.message());
            return false;
        }

        std::ofstream out(target, std::ios::binary);
        if (!out) {
            error = "Failed to open extension data file for writing: " + target.string();
            return false;
        }
        if (!data.empty())
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            error = "Failed to write extension data file: " + target.string();
            return false;
        }
        return true;
    }

    FilesystemProjectSerializationReadContext::FilesystemProjectSerializationReadContext(
        std::filesystem::path projectFile,
        std::filesystem::path projectDir)
        : project_file_(std::move(projectFile))
        , project_dir_(std::move(projectDir)) {
    }

    std::filesystem::path FilesystemProjectSerializationReadContext::projectFile() const {
        return project_file_;
    }

    std::filesystem::path FilesystemProjectSerializationReadContext::projectDirectory() const {
        return project_dir_;
    }

    std::optional<std::vector<uint8_t>> FilesystemProjectSerializationReadContext::readExtensionFile(
        std::string_view extensionId,
        const std::filesystem::path& relativePath,
        std::string& error) {
        if (!isSafeRelativeExtensionPath(relativePath)) {
            error = "Extension attempted to read an unsafe project-relative path.";
            return std::nullopt;
        }

        auto target = (extensionDataRoot(project_dir_, extensionId) / relativePath).lexically_normal();
        std::ifstream in(target, std::ios::binary);
        if (!in) {
            error = "Failed to open extension data file for reading: " + target.string();
            return std::nullopt;
        }

        std::vector<char> bytes{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        if (!in.eof()) {
            error = "Failed to read extension data file: " + target.string();
            return std::nullopt;
        }
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    }

} // namespace uapmd::sequencer_detail
