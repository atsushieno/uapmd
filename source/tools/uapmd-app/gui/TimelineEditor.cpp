#include <cctype>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <format>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <limits>
#include "PlatformDialogs.hpp"

#include <imgui.h>
#include <umppi/umppi.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-data/detail/timeline/MidiClipSourceNode.hpp"

#include "TimelineEditor.hpp"
#include "ClipPreview.hpp"
#include "../DocumentProviderHelpers.hpp"
#include "FontIcons.hpp"
#include "../AppModel.hpp"

namespace uapmd::gui {

namespace {
constexpr int32_t kMasterTrackClipId = -1000;
constexpr double kDisplayDefaultBpm = 120.0;

struct ClipKey {
    std::string referenceId;

    bool operator==(const ClipKey& other) const {
        return referenceId == other.referenceId;
    }
};

struct ClipKeyHash {
    size_t operator()(const ClipKey& key) const {
        return std::hash<std::string>{}(key.referenceId);
    }
};

uint64_t mixHash(uint64_t seed, uint64_t value) {
    constexpr uint64_t kPrime = 1099511628211ull;
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    seed *= kPrime;
    return seed;
}

uint64_t midiSourceFingerprint(const uapmd::MidiClipSourceNode& midiSource) {
    const auto& words = midiSource.umpEvents();
    const auto& ticks = midiSource.eventTimestampsTicks();

    uint64_t hash = 1469598103934665603ull;
    hash = mixHash(hash, words.size());
    hash = mixHash(hash, ticks.size());
    hash = mixHash(hash, static_cast<uint64_t>(midiSource.tickResolution()));
    hash = mixHash(hash, static_cast<uint64_t>(midiSource.clipTempo() * 1000.0));

    const size_t sampleCount = std::min<size_t>(words.size(), 24);
    for (size_t i = 0; i < sampleCount; ++i) {
        size_t index = (sampleCount <= 1 || words.size() <= 1)
            ? 0
            : (i * (words.size() - 1)) / (sampleCount - 1);
        hash = mixHash(hash, words[index]);
        if (index < ticks.size())
            hash = mixHash(hash, ticks[index]);
    }

    return hash;
}

int32_t toTimelineFrame(double units) {
    if (!std::isfinite(units))
        return 0;
    const double maxUnits = static_cast<double>(std::numeric_limits<int32_t>::max() - 1);
    const double clamped = std::clamp(units, 0.0, maxUnits);
    return static_cast<int32_t>(std::llround(clamped));
}

std::vector<MidiDumpWindow::EventRow> buildMidiDumpRows(
    const uapmd::MidiClipReader::ClipInfo& clipInfo,
    uint32_t tickResolution,
    double tempo
) {
    std::vector<MidiDumpWindow::EventRow> rows;
    if (clipInfo.ump_data.empty())
        return rows;

    const double safeResolution = tickResolution > 0 ? static_cast<double>(tickResolution) : 1.0;
    const double safeTempo = tempo > 0.0 ? tempo : 120.0;

    size_t index = 0;
    rows.reserve(clipInfo.ump_data.size());
    while (index < clipInfo.ump_data.size()) {
        const uint32_t word = clipInfo.ump_data[index];
        const size_t byteLength = umppi::Ump{word}.getSizeInBytes();
        size_t wordCount = (byteLength + sizeof(uint32_t) - 1) / sizeof(uint32_t);
        wordCount = std::max<size_t>(1, wordCount);
        const size_t availableWords = clipInfo.ump_data.size() - index;
        if (wordCount > availableWords)
            wordCount = availableWords;

        double ticks = 0.0;
        if (index < clipInfo.ump_tick_timestamps.size())
            ticks = static_cast<double>(clipInfo.ump_tick_timestamps[index]);
        const double beats = ticks / safeResolution;
        const double seconds = beats * (60.0 / safeTempo);

        MidiDumpWindow::EventRow row;
        row.timeSeconds = seconds;
        row.tickPosition = static_cast<uint64_t>(ticks);
        const uint64_t prevTick = rows.empty() ? 0 : rows.back().tickPosition;
        row.deltaTicks = row.tickPosition > prevTick ? row.tickPosition - prevTick : 0;
        row.timeLabel = std::format("{:.3f}s [{}]", seconds, row.tickPosition);

        bool firstByte = true;
        for (size_t offset = 0; offset < wordCount; ++offset) {
            const uint32_t dataWord = clipInfo.ump_data[index + offset];
            for (int shift = 24; shift >= 0; shift -= 8) {
                const uint8_t byteValue = static_cast<uint8_t>((dataWord >> shift) & 0xFF);
                if (!firstByte)
                    row.hexBytes.push_back(' ');
                firstByte = false;
                row.hexBytes += std::format("{:02X}", byteValue);
            }
        }

        rows.push_back(std::move(row));
        index += wordCount;
    }

    return rows;
}

bool hasExtension(std::string_view ext, std::initializer_list<std::string_view> expected) {
    for (auto candidate : expected) {
        if (ext == candidate)
            return true;
    }
    return false;
}

std::string lowerCaseExtension(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool looksLikeMidiFile(const std::string& path, std::string_view ext) {
    if (hasExtension(ext, {".mid", ".midi", ".smf", ".midi2"}))
        return true;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    char header[4]{};
    file.read(header, sizeof(header));
    return file.gcount() == 4 && std::memcmp(header, "MThd", 4) == 0;
}

bool looksLikeAudioFile(std::string_view ext) {
    return hasExtension(ext, {".wav", ".flac", ".ogg"});
}
}  // namespace

TimelineEditor::TimelineEditor() {
    // Set up PluginSelector callbacks
    pluginSelector_.setOnInstantiatePlugin([this](const std::string& format, const std::string& pluginId, int32_t trackIndex) {
        // Create plugin instance through AppModel
        auto& appModel = uapmd::AppModel::instance();
        uapmd::AppModel::PluginInstanceConfig config;
        appModel.createPluginInstanceAsync(format, pluginId, trackIndex, config);
        showPluginSelectorWindow_ = false;
    });

    pluginSelector_.setOnScanPlugins([](bool forceRescan, bool useRemoteProcess, double remoteTimeoutSeconds) {
        uapmd::AppModel::instance().performPluginScanning(
            forceRescan,
            useRemoteProcess
                ? uapmd::AppModel::PluginScanRequest::RemoteProcess
                : uapmd::AppModel::PluginScanRequest::InProcess,
            remoteTimeoutSeconds);
    });

    refreshAllSequenceEditorTracks();
}

void TimelineEditor::setCallbacks(TimelineEditorCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void TimelineEditor::setChildWindowSizeHelper(
    std::function<void(const std::string&, ImVec2)> setSize,
    std::function<void(const std::string&)> updateSize
) {
    setNextChildWindowSize_ = std::move(setSize);
    updateChildWindowSizeState_ = std::move(updateSize);
}

void TimelineEditor::update() {
    // Currently empty - reserved for future frame updates
}

SequenceEditor::RenderContext TimelineEditor::buildRenderContext(float uiScale) {
    return SequenceEditor::RenderContext{
        .refreshClips = [this](int32_t trackIndex) {
            refreshSequenceEditorForTrack(trackIndex);
        },
        .addClip = [this](int32_t trackIndex, const std::string& filepath) {
            addClipToTrack(trackIndex, filepath);
        },
        .addClipAtPosition = [this](int32_t trackIndex, const std::string& filepath, double positionSeconds) {
            addClipToTrackAtPosition(trackIndex, filepath, positionSeconds);
        },
        .addAudioClip = [this](int32_t trackIndex) {
            addAudioClipToTrack(trackIndex);
        },
        .addSmfClip = [this](int32_t trackIndex) {
            addSmfClipToTrack(trackIndex);
        },
        .addSmf2Clip = [this](int32_t trackIndex) {
            addSmf2ClipToTrack(trackIndex);
        },
        .addBlankMidiClipAtPosition = [this](int32_t trackIndex, double positionSeconds) {
            addBlankMidi2ClipToTrackAtPosition(trackIndex, positionSeconds);
        },
        .removeClip = [this](int32_t trackIndex, int32_t clipId) {
            removeClipFromTrack(trackIndex, clipId);
        },
        .clearAllClips = [this](int32_t trackIndex) {
            clearAllClipsFromTrack(trackIndex);
        },
        .updateClip = [this](int32_t trackIndex, int32_t clipId, const std::string& anchorReferenceId, const std::string& origin, const std::string& position) {
            updateClip(trackIndex, clipId, anchorReferenceId, origin, position);
        },
        .updateClipName = [this](int32_t trackIndex, int32_t clipId, const std::string& name) {
            updateClipName(trackIndex, clipId, name);
        },
        .changeClipFile = [this](int32_t trackIndex, int32_t clipId) {
            changeClipFile(trackIndex, clipId);
        },
        .moveClipAbsolute = [this](int32_t trackIndex, int32_t clipId, double seconds) {
            moveClipAbsolute(trackIndex, clipId, seconds);
        },
        .showMidiClipDump = [this](int32_t trackIndex, int32_t clipId) {
            showMidiClipDump(trackIndex, clipId);
        },
        .showPianoRoll = [this](int32_t trackIndex, int32_t clipId) {
            showPianoRoll(trackIndex, clipId);
        },
        .showMasterTrackDump = [this]() {
            showMasterMetaDump();
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .secondsToTimelineUnits = [this](double seconds) {
            return secondsToTimelineUnits(seconds);
        },
        .timelineUnitsToSeconds = [this](double units) {
            return timelineUnitsToSeconds(units);
        },
        .timelineUnitsLabel = timelineUnitsLabel_.c_str(),
        .uiScale = uiScale,
    };
}

void TimelineEditor::render(float uiScale) {
    syncExternalTimelineChanges();
    auto context = buildRenderContext(uiScale);
    renderTrackList(context);
    sequenceEditor_.render(context);

    // Render InstanceDetails with context
    InstanceDetails::RenderContext detailsContext{
        .buildTrackInstance = [this](int32_t instanceId) -> std::optional<TrackInstance> {
            if (callbacks_.buildTrackInstanceInfo)
                return callbacks_.buildTrackInstanceInfo(instanceId);
            return std::nullopt;
        },
        .savePluginState = [this](int32_t instanceId) {
            if (callbacks_.savePluginState)
                callbacks_.savePluginState(instanceId);
        },
        .loadPluginState = [this](int32_t instanceId) {
            if (callbacks_.loadPluginState)
                callbacks_.loadPluginState(instanceId);
        },
        .removeInstance = [this](int32_t instanceId) {
            if (callbacks_.handleRemoveInstance)
                callbacks_.handleRemoveInstance(instanceId);
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .onWindowClosed = [this](int32_t instanceId) {
            if (callbacks_.onInstanceDetailsClosed)
                callbacks_.onInstanceDetailsClosed(instanceId);
        },
        .uiScale = uiScale,
    };
    instanceDetails_.render(detailsContext);

    // Render MidiDumpWindow with context
    MidiDumpWindow::RenderContext midiDumpContext{
        .reloadClip = [this](int32_t trackIndex, int32_t clipId) {
            return buildMidiClipDumpData(trackIndex, clipId);
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            if (setNextChildWindowSize_)
                setNextChildWindowSize_(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            if (updateChildWindowSizeState_)
                updateChildWindowSizeState_(id);
        },
        .applyEdits = [this](const MidiDumpWindow::EditPayload& payload, std::string& error) {
            return applyMidiClipEdits(payload, error);
        },
        .uiScale = uiScale,
    };
    midiDumpWindow_.render(midiDumpContext);

    PianoRollEditor::RenderContext pianoCtx;
    pianoCtx.uiScale = uiScale;
    pianoCtx.applyEdits = [this](int32_t trackIndex, int32_t clipId,
                                  std::vector<uapmd_ump_t> newEvents,
                                  std::vector<uint64_t>    newTicks,
                                  std::string&             error) -> bool {
        return applyPianoRollEdits(trackIndex, clipId,
                                   std::move(newEvents), std::move(newTicks), error);
    };
    pianoCtx.reloadPreview = [](int32_t trackIndex, int32_t clipId) -> std::shared_ptr<ClipPreview> {
        auto& appModel = uapmd::AppModel::instance();
        auto tracks = appModel.getTimelineTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) ||
                !tracks[trackIndex])
            return nullptr;
        auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
        if (!clip) return nullptr;
        return createMidiClipPreview(trackIndex, *clip, 0.0);
    };
    pianoCtx.previewNoteOn = [](int32_t trackIndex, int midiNote) {
        auto& seq = uapmd::AppModel::instance().sequencer();
        auto tracksRef = seq.engine()->tracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracksRef.size())) return;
        auto* track = tracksRef[trackIndex];
        if (!track) return;
        for (int32_t instanceId : track->orderedInstanceIds())
            if (seq.engine()->getPluginInstance(instanceId))
                seq.engine()->sendNoteOn(instanceId, midiNote);
    };
    pianoCtx.previewNoteOff = [](int32_t trackIndex, int midiNote) {
        auto& seq = uapmd::AppModel::instance().sequencer();
        auto tracksRef = seq.engine()->tracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracksRef.size())) return;
        auto* track = tracksRef[trackIndex];
        if (!track) return;
        for (int32_t instanceId : track->orderedInstanceIds())
            if (seq.engine()->getPluginInstance(instanceId))
                seq.engine()->sendNoteOff(instanceId, midiNote);
    };
    pianoCtx.getTrackPluginParameters = [this](int32_t trackIndex) {
        return getPluginParametersForTrack(trackIndex);
    };
    pianoRollEditor_.render(pianoCtx);
}

void TimelineEditor::renderPluginSelectorWindow(float uiScale) {
    if (!showPluginSelectorWindow_)
        return;

    const std::string windowId = "PluginSelector";
    if (setNextChildWindowSize_)
        setNextChildWindowSize_(windowId, ImVec2(720.0f, 600.0f));
    if (ImGui::Begin("Plugin Selector", &showPluginSelectorWindow_)) {
        if (updateChildWindowSizeState_)
            updateChildWindowSizeState_(windowId);

        pluginSelector_.setScanning(uapmd::AppModel::instance().isScanning());
        pluginSelector_.render();
    }
    ImGui::End();
}

void TimelineEditor::renderTrackList(const SequenceEditor::RenderContext& context) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    ImGui::TextUnformatted("Track List");
    ImGui::Spacing();

    ImGui::BeginChild("TrackListScroll", ImVec2(0, 0), true, ImGuiWindowFlags_None);
    renderMasterTrackRow(context);
    ImGui::Spacing();

    if (tracks.empty()) {
        ImGui::TextDisabled("No tracks available.");
        ImGui::Spacing();
    }

    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
        if (appModel.isTrackHidden(i))
            continue;
        renderTrackRow(i, context);
        ImGui::Spacing();
    }

    if (ImGui::Button(icons::Plus)) {
        int32_t newIndex = appModel.addTrack();
        if (newIndex >= 0)
            refreshSequenceEditorForTrack(newIndex);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add track");
    if (callbacks_.showPluginInstances) {
        ImGui::SameLine();
        if (ImGui::Button("Plugin Instances"))
            callbacks_.showPluginInstances();
    }
    ImGui::EndChild();
}

void TimelineEditor::renderMasterTrackRow(const SequenceEditor::RenderContext& context) {
    auto snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
        uapmd::AppModel::instance().buildMasterTrackSnapshot());

    const double lastTempoTime = snapshot->tempoPoints.empty()
        ? 0.0 : snapshot->tempoPoints.back().timeSeconds;
    const double lastTempoBpm = snapshot->tempoPoints.empty()
        ? 0.0 : snapshot->tempoPoints.back().bpm;
    const double lastSigTime = snapshot->timeSignaturePoints.empty()
        ? 0.0 : snapshot->timeSignaturePoints.back().timeSeconds;
    const double lastSigNum = snapshot->timeSignaturePoints.empty()
        ? 0.0 : snapshot->timeSignaturePoints.back().signature.numerator;

    const std::string signature = std::format("{}:{}:{:.6f}:{:.6f}:{:.6f}:{:.6f}:{:.6f}",
        snapshot->tempoPoints.size(),
        snapshot->timeSignaturePoints.size(),
        snapshot->maxTimeSeconds,
        lastTempoTime,
        lastTempoBpm,
        lastSigTime,
        lastSigNum);

    if (signature != masterTrackSignature_) {
        masterTrackSignature_ = signature;
        masterTrackSnapshot_ = snapshot;
        rebuildTempoSegments(masterTrackSnapshot_);

        std::vector<SequenceEditor::ClipRow> rows;
        SequenceEditor::ClipRow row;
        row.clipId = kMasterTrackClipId;
        row.trackReferenceId = "master_track";
        row.anchorReferenceId.clear();
        row.anchorOrigin = "Start";
        row.position = "+0.000s";
        row.isMidiClip = false;
        row.isMasterTrack = true;
        row.name = snapshot->empty() ? "No Meta Events" : "SMF Meta Events";
        row.filename = "-";
        row.filepath = "";
        const double durationSeconds = std::max(1.0, snapshot->maxTimeSeconds);
        row.duration = std::format("{:.3f}s", durationSeconds);
        row.timelineStart = toTimelineFrame(secondsToTimelineUnits(0.0));
        row.timelineEnd = std::max(row.timelineStart + 1, toTimelineFrame(secondsToTimelineUnits(durationSeconds)));
        std::vector<ClipPreview::TempoPoint> tempoPoints;
        tempoPoints.reserve(snapshot->tempoPoints.size());
        for (const auto& point : snapshot->tempoPoints)
            tempoPoints.push_back(ClipPreview::TempoPoint{point.timeSeconds, point.bpm});
        std::vector<ClipPreview::TimeSignaturePoint> sigPoints;
        sigPoints.reserve(snapshot->timeSignaturePoints.size());
        for (const auto& sig : snapshot->timeSignaturePoints) {
            sigPoints.push_back(ClipPreview::TimeSignaturePoint{
                sig.timeSeconds,
                sig.signature.numerator,
                sig.signature.denominator
            });
        }
        row.customPreview = createMasterMetaPreview(std::move(tempoPoints), std::move(sigPoints), durationSeconds);
        rows.push_back(std::move(row));

        sequenceEditor_.refreshClips(uapmd::kMasterTrackIndex, rows);

        // Refresh all regular tracks since tempo segments changed
        auto& appModel = uapmd::AppModel::instance();
        auto tracks = appModel.getTimelineTracks();
        for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
            if (!appModel.isTrackHidden(i))
                refreshSequenceEditorForTrack(i);
        }
    } else {
        masterTrackSnapshot_ = snapshot;
    }

    ImGui::PushID("MasterTrackRow");
    if (ImGui::BeginTable("##MasterTrackTable", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed, 160.0f * context.uiScale);
        ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Separator();
        ImGui::TextUnformatted("Master Track");
        ImGui::Spacing();
        if (ImGui::Button("Clips..."))
            sequenceEditor_.showWindow(uapmd::kMasterTrackIndex);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto* masterTrack = sequencer.engine()->masterTrack();
        std::vector<int32_t> validInstances;
        if (masterTrack) {
            auto& ids = masterTrack->orderedInstanceIds();
            validInstances.reserve(ids.size());
            for (int32_t instanceId : ids) {
                if (sequencer.engine()->getPluginInstance(instanceId))
                    validInstances.push_back(instanceId);
            }
        }

        std::string pluginLabel = "Add Plugin";
        if (!validInstances.empty()) {
            if (auto* instance = sequencer.engine()->getPluginInstance(validInstances.front()))
                pluginLabel = instance->displayName();
        }

        std::string pluginPopupId = "MasterTrackActions";
        if (ImGui::Button(std::format("{}...", pluginLabel).c_str()))
            ImGui::OpenPopup(pluginPopupId.c_str());
        if (ImGui::BeginPopup(pluginPopupId.c_str())) {
            if (masterTrack) {
                for (int i = 0; i < static_cast<int>(validInstances.size()); ++i) {
                    int32_t instanceId = validInstances[static_cast<size_t>(i)];
                    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
                    if (!instance)
                        continue;
                    std::string pluginName = instance->displayName();

                    bool detailsVisible = instanceDetails_.isVisible(instanceId);
                    std::string detailsLabel = std::format("{} {} Details##details{}",
                                                           detailsVisible ? "Hide" : "Show",
                                                           pluginName,
                                                           instanceId);
                    if (ImGui::MenuItem(detailsLabel.c_str())) {
                        if (detailsVisible)
                            instanceDetails_.hideWindow(instanceId);
                        else
                            instanceDetails_.showWindow(instanceId);
                    }

                    // Show|Hide GUI button
                    if (callbacks_.buildTrackInstanceInfo) {
                        if (auto trackInstance = callbacks_.buildTrackInstanceInfo(instanceId)) {
                            bool disableShowUIButton = !trackInstance->hasUI;
                            if (disableShowUIButton)
                                ImGui::BeginDisabled();
                            std::string uiLabel = std::format("{} {} GUI##gui{}",
                                                              trackInstance->uiVisible ? "Hide" : "Show",
                                                              pluginName,
                                                              instanceId);
                            if (ImGui::MenuItem(uiLabel.c_str())) {
                                if (trackInstance->uiVisible)
                                    uapmd::AppModel::instance().hidePluginUI(instanceId);
                                else
                                    uapmd::AppModel::instance().requestShowPluginUI(instanceId);
                            }
                            if (disableShowUIButton)
                                ImGui::EndDisabled();
                        }
                    }
                }

                if (!validInstances.empty()) {
                    ImGui::Separator();
                    for (int i = 0; i < static_cast<int>(validInstances.size()); ++i) {
                        int32_t instanceId = validInstances[static_cast<size_t>(i)];
                        auto* instance = sequencer.engine()->getPluginInstance(instanceId);
                        if (!instance)
                            continue;
                        std::string pluginName = instance->displayName();

                        std::string deleteLabel = std::format("Delete {} (at [{}])##delete{}",
                                                              pluginName,
                                                              i + 1,
                                                              instanceId);
                        if (ImGui::MenuItem(deleteLabel.c_str())) {
                            if (callbacks_.handleRemoveInstance)
                                callbacks_.handleRemoveInstance(instanceId);
                        }
                    }
                    ImGui::Separator();
                }
            }

            if (ImGui::MenuItem("Add Plugin")) {
                pluginSelector_.setTargetMasterTrack(uapmd::kMasterTrackIndex);
                showPluginSelectorWindow_ = true;
            }
            ImGui::EndPopup();
        }

        ImGui::TableSetColumnIndex(1);
        const float timelineHeight = sequenceEditor_.getInlineTimelineHeight(uapmd::kMasterTrackIndex, context.uiScale);
        sequenceEditor_.renderTimelineInline(uapmd::kMasterTrackIndex, context, timelineHeight);

        ImGui::EndTable();
    }
    ImGui::PopID();
}

void TimelineEditor::renderTrackRow(int32_t trackIndex, const SequenceEditor::RenderContext& context) {
    ImGui::PushID(trackIndex);
    if (ImGui::BeginTable("##TrackRowTable", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed, 160.0f * context.uiScale);
        ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        // Control column
        ImGui::TableSetColumnIndex(0);
        ImGui::Separator();
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto tracksRef = sequencer.engine()->tracks();
        SequencerTrack* track = (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracksRef.size()))
            ? tracksRef[trackIndex]
            : nullptr;
        std::vector<int32_t> validInstances;
        if (track) {
            auto& ids = track->orderedInstanceIds();
            validInstances.reserve(ids.size());
            for (int32_t instanceId : ids) {
                if (sequencer.engine()->getPluginInstance(instanceId))
                    validInstances.push_back(instanceId);
            }
        }
        std::string pluginLabel = "Add Plugin";
        if (!validInstances.empty()) {
            if (auto* instance = sequencer.engine()->getPluginInstance(validInstances.front()))
                pluginLabel = instance->displayName();
        }

        std::string popupId = std::format("TrackActions##{}", trackIndex);
        std::string clipPopupId = std::format("ClipActions##{}", trackIndex);

        if (ImGui::Button("Clips..."))
            ImGui::OpenPopup(clipPopupId.c_str());
        if (track) {
            bool bypassed = track->bypassed();
            if (bypassed)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const char* toggleIcon = bypassed ? uapmd::gui::icons::ToggleOff : uapmd::gui::icons::ToggleOn;
            std::string toggleLabel = std::format("{}##TrackBypass{}", toggleIcon, trackIndex);
            if (ImGui::Button(toggleLabel.c_str()))
                track->bypassed(!bypassed);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(bypassed ? "Track bypassed (click to enable)" : "Bypass track");
            if (bypassed)
                ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        if (ImGui::Button(uapmd::gui::icons::DeleteTrack))
            deleteTrack(trackIndex);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Delete track");

        if (ImGui::Button(std::format("{}...", pluginLabel).c_str()))
            ImGui::OpenPopup(popupId.c_str());
        if (ImGui::BeginPopup(clipPopupId.c_str())) {
            if (ImGui::MenuItem("Edit Clips...", nullptr, sequenceEditor_.isVisible(trackIndex))) {
                sequenceEditor_.showWindow(trackIndex);
                refreshSequenceEditorForTrack(trackIndex);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Clip"))
                addBlankMidi2ClipToTrack(trackIndex);
            ImGui::Separator();
            if (ImGui::MenuItem("Import Audio Clip..."))
                addAudioClipToTrack(trackIndex);
            if (ImGui::MenuItem("Import SMF as Clip..."))
                addSmfClipToTrack(trackIndex);
            if (ImGui::MenuItem("Import SMF2Clip..."))
                addSmf2ClipToTrack(trackIndex);
            ImGui::Separator();
            if (ImGui::MenuItem("Clear All"))
                context.clearAllClips(trackIndex);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup(popupId.c_str())) {
            if (track) {
                for (int i = 0; i < static_cast<int>(validInstances.size()); ++i) {
                    int32_t instanceId = validInstances[static_cast<size_t>(i)];
                    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
                    if (!instance)
                        continue;
                    std::string pluginName = instance->displayName();

                    bool detailsVisible = instanceDetails_.isVisible(instanceId);
                    std::string detailsLabel = std::format("{} {} Details##details{}",
                                                           detailsVisible ? "Hide" : "Show",
                                                           pluginName,
                                                           instanceId);
                    if (ImGui::MenuItem(detailsLabel.c_str())) {
                        if (detailsVisible)
                            instanceDetails_.hideWindow(instanceId);
                        else
                            instanceDetails_.showWindow(instanceId);
                    }

                    // Show|Hide GUI button
                    if (callbacks_.buildTrackInstanceInfo) {
                        if (auto trackInstance = callbacks_.buildTrackInstanceInfo(instanceId)) {
                            bool disableShowUIButton = !trackInstance->hasUI;
                            if (disableShowUIButton)
                                ImGui::BeginDisabled();
                            std::string uiLabel = std::format("{} {} GUI##gui{}",
                                                              trackInstance->uiVisible ? "Hide" : "Show",
                                                              pluginName,
                                                              instanceId);
                            if (ImGui::MenuItem(uiLabel.c_str())) {
                                if (trackInstance->uiVisible)
                                    uapmd::AppModel::instance().hidePluginUI(instanceId);
                                else
                                    uapmd::AppModel::instance().requestShowPluginUI(instanceId);
                            }
                            if (disableShowUIButton)
                                ImGui::EndDisabled();
                        }
                    }
                }

                if (!validInstances.empty()) {
                    ImGui::Separator();
                    for (int i = 0; i < static_cast<int>(validInstances.size()); ++i) {
                        int32_t instanceId = validInstances[static_cast<size_t>(i)];
                        auto* instance = sequencer.engine()->getPluginInstance(instanceId);
                        if (!instance)
                            continue;
                        std::string pluginName = instance->displayName();

                        std::string deleteLabel = std::format("Delete {} (at [{}])##delete{}",
                                                              pluginName,
                                                              i + 1,
                                                              instanceId);
                        if (ImGui::MenuItem(deleteLabel.c_str())) {
                            if (callbacks_.handleRemoveInstance)
                                callbacks_.handleRemoveInstance(instanceId);
                        }
                    }
                    ImGui::Separator();
                }
            }

            if (ImGui::MenuItem("Add Plugin")) {
                pluginSelector_.setTargetTrackIndex(trackIndex);
                showPluginSelectorWindow_ = true;
            }

            ImGui::EndPopup();
        }

        // Timeline column
        ImGui::TableSetColumnIndex(1);
        const float timelineHeight = sequenceEditor_.getInlineTimelineHeight(trackIndex, context.uiScale);
        sequenceEditor_.renderTimelineInline(trackIndex, context, timelineHeight);

        ImGui::EndTable();
    }
    ImGui::PopID();
}

void TimelineEditor::deleteTrack(int32_t trackIndex) {
    uapmd::AppModel::instance().removeTrack(trackIndex);
}

void TimelineEditor::refreshAllSequenceEditorTracks() {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
        if (appModel.isTrackHidden(i))
            continue;
        refreshSequenceEditorForTrack(i);
    }
}

void TimelineEditor::syncExternalTimelineChanges() {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    for (auto it = trackContentSignatures_.begin(); it != trackContentSignatures_.end();) {
        if (it->first < 0 || it->first >= static_cast<int32_t>(tracks.size()))
            it = trackContentSignatures_.erase(it);
        else
            ++it;
    }

    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
        if (appModel.isTrackHidden(i))
            continue;
        const auto signature = buildTrackContentSignature(i);
        auto it = trackContentSignatures_.find(i);
        if (it != trackContentSignatures_.end() && it->second == signature)
            continue;
        refreshSequenceEditorForTrack(i);
    }
}

void TimelineEditor::handleTrackLayoutChange(const uapmd::AppModel::TrackLayoutChange& change) {
    switch (change.type) {
        case uapmd::AppModel::TrackLayoutChange::Type::Added:
            refreshSequenceEditorForTrack(change.trackIndex);
            break;
        case uapmd::AppModel::TrackLayoutChange::Type::Removed:
            sequenceEditor_.hideWindow(change.trackIndex);
            trackContentSignatures_.erase(change.trackIndex);
            break;
        case uapmd::AppModel::TrackLayoutChange::Type::Cleared:
            sequenceEditor_.reset();
            trackContentSignatures_.clear();
            break;
    }
}

void TimelineEditor::rebuildTempoSegments(const std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot>& snapshot) {
    tempoSegments_.clear();
    if (!snapshot || snapshot->tempoPoints.empty()) {
        timelineUnitsLabel_ = "seconds";
        return;
    }

    const auto& tempoPoints = snapshot->tempoPoints;
    double currentBpm = tempoPoints.front().bpm > 0.0 ? tempoPoints.front().bpm : kDisplayDefaultBpm;
    double lastTime = 0.0;
    double accumulatedBeats = 0.0;

    for (const auto& point : tempoPoints) {
        double eventTime = std::max(0.0, point.timeSeconds);
        if (eventTime > lastTime) {
            const double bpmToUse = currentBpm > 0.0 ? currentBpm : kDisplayDefaultBpm;
            tempoSegments_.push_back(TempoSegment{lastTime, eventTime, bpmToUse, accumulatedBeats});
            accumulatedBeats += (eventTime - lastTime) * (bpmToUse / 60.0);
            lastTime = eventTime;
        }
        if (point.bpm > 0.0)
            currentBpm = point.bpm;
    }

    const double bpmToUse = currentBpm > 0.0 ? currentBpm : kDisplayDefaultBpm;
    tempoSegments_.push_back(TempoSegment{
        lastTime,
        std::numeric_limits<double>::infinity(),
        bpmToUse,
        accumulatedBeats
    });
    timelineUnitsLabel_ = "beats";

    // Debug: log tempo segments
    Logger::global()->logDiagnostic("[TEMPO SEGMENTS] Built %d segments:", tempoSegments_.size());
    for (size_t i = 0; i < tempoSegments_.size(); ++i) {
        const auto& seg = tempoSegments_[i];
        Logger::global()->logDiagnostic("  [%d] time=(%.2f, %.2f) bpm=%.2f accumulatedBeats=%.2f",
            i, seg.startTime, seg.endTime, seg.bpm, seg.accumulatedBeats);
    }
}

double TimelineEditor::secondsToTimelineUnits(double seconds) const {
    if (tempoSegments_.empty())
        return std::max(0.0, seconds);

    const double clampedSeconds = std::max(0.0, seconds);
    for (const auto& segment : tempoSegments_) {
        if (clampedSeconds < segment.endTime) {
            const double bpm = segment.bpm > 0.0 ? segment.bpm : kDisplayDefaultBpm;
            return segment.accumulatedBeats + (clampedSeconds - segment.startTime) * (bpm / 60.0);
        }
    }

    const auto& last = tempoSegments_.back();
    const double bpm = last.bpm > 0.0 ? last.bpm : kDisplayDefaultBpm;
    return last.accumulatedBeats + (clampedSeconds - last.startTime) * (bpm / 60.0);
}

double TimelineEditor::timelineUnitsToSeconds(double units) const {
    if (tempoSegments_.empty())
        return std::max(0.0, units);

    const double clampedUnits = std::max(0.0, units);
    for (const auto& segment : tempoSegments_) {
        const double bpm = segment.bpm > 0.0 ? segment.bpm : kDisplayDefaultBpm;
        double segmentEndBeats = std::numeric_limits<double>::infinity();
        if (std::isfinite(segment.endTime))
            segmentEndBeats = segment.accumulatedBeats + (segment.endTime - segment.startTime) * (bpm / 60.0);

        if (clampedUnits < segmentEndBeats)
            return segment.startTime + (clampedUnits - segment.accumulatedBeats) * (60.0 / bpm);
    }

    const auto& last = tempoSegments_.back();
    const double bpm = last.bpm > 0.0 ? last.bpm : kDisplayDefaultBpm;
    return last.startTime + (clampedUnits - last.accumulatedBeats) * (60.0 / bpm);
}

void TimelineEditor::invalidateMasterTrackSnapshot() {
    masterTrackSnapshot_.reset();
    masterTrackSignature_.clear();
    tempoSegments_.clear();
    timelineUnitsLabel_ = "seconds";
}

void TimelineEditor::refreshSequenceEditorForTrack(int32_t trackIndex) {
    if (trackIndex < 0)
        return;
    auto& appModel = uapmd::AppModel::instance();

    // Ensure tempo segments are built before computing clip positions
    if (tempoSegments_.empty()) {
        auto snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
            appModel.buildMasterTrackSnapshot());
        rebuildTempoSegments(snapshot);
    }
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    auto* track = tracks[trackIndex];
    auto clips = track->clipManager().getAllClips();

    // Sort clips by clipId to ensure chronological order
    std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
        return a.clipId > b.clipId;
    });

    std::vector<SequenceEditor::ClipRow> displayClips;
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

    for (const auto& clip : clips) {
        SequenceEditor::ClipRow row;
        row.clipId = clip.clipId;
        row.referenceId = clip.referenceId;
        row.trackReferenceId = track->referenceId();
        row.anchorReferenceId = clip.anchorReferenceId;

        row.anchorOrigin = (clip.anchorOrigin == uapmd::AnchorOrigin::Start) ? "Start" : "End";

        double positionSeconds = clip.anchorOffset.toSeconds(appModel.sampleRate());
        row.position = std::format("{:+.3f}s", positionSeconds);

        double durationSeconds = static_cast<double>(clip.durationSamples) / appModel.sampleRate();
        row.duration = std::format("{:.3f}s", durationSeconds);

        row.name = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
        row.filepath = clip.filepath;
        if (clip.filepath.empty()) {
            row.filename = "(no file)";
        } else {
            size_t lastSlash = clip.filepath.find_last_of("/\\");
            row.filename = (lastSlash != std::string::npos)
                ? clip.filepath.substr(lastSlash + 1)
                : clip.filepath;
        }

        row.isMidiClip = (clip.clipType == uapmd::ClipType::Midi);
        if (row.isMidiClip)
            row.mimeType = "audio/midi";
        else
            row.mimeType = "";

        auto absolutePosition = clip.position;
        double absoluteStartSeconds = static_cast<double>(absolutePosition.samples) / sampleRate;
        double durationSecondsExact = static_cast<double>(clip.durationSamples) / sampleRate;
        const double startUnits = secondsToTimelineUnits(absoluteStartSeconds);
        const double endUnits = secondsToTimelineUnits(absoluteStartSeconds + durationSecondsExact);
        row.timelineStart = toTimelineFrame(startUnits);
        int32_t endFrame = toTimelineFrame(endUnits);
        if (endFrame <= row.timelineStart)
            endFrame = row.timelineStart + 1;
        row.timelineEnd = endFrame;

        displayClips.push_back(row);
    }

    sequenceEditor_.refreshClips(trackIndex, displayClips);
    trackContentSignatures_[trackIndex] = buildTrackContentSignature(trackIndex);
}

std::string TimelineEditor::buildTrackContentSignature(int32_t trackIndex) const {
    auto tracks = uapmd::AppModel::instance().getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex])
        return {};

    auto* track = tracks[trackIndex];
    auto clips = track->clipManager().getAllClips();
    std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
        return a.clipId < b.clipId;
    });

    std::string signature = std::to_string(clips.size());
    signature.reserve(signature.size() + clips.size() * 96);
    for (const auto& clip : clips) {
        uint64_t midiHash = 0;
        auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
        if (auto* midiSource = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get()))
            midiHash = midiSourceFingerprint(*midiSource);

        signature += std::format("|{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            clip.clipId,
            static_cast<int>(clip.clipType),
            clip.position.samples,
            clip.durationSamples,
            clip.sourceNodeInstanceId,
            std::hash<std::string>{}(clip.referenceId),
            std::hash<std::string>{}(clip.anchorReferenceId),
            static_cast<int>(clip.anchorOrigin),
            clip.anchorOffset.samples,
            clip.tickResolution,
            clip.name,
            midiHash);
    }
    return signature;
}

void TimelineEditor::resolveAllClipAnchors() {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    struct ClipRecord {
        uapmd::ClipManager* clipManager{nullptr};
        uapmd::ClipData clip;
    };

    std::unordered_map<ClipKey, ClipRecord, ClipKeyHash> clipRecords;
    clipRecords.reserve(128);

    auto collectTrack = [&clipRecords](uapmd::TimelineTrack* track) {
        if (!track)
            return;
        auto clips = track->clipManager().getAllClips();
        for (const auto& clip : clips)
            clipRecords.emplace(ClipKey{clip.referenceId}, ClipRecord{&track->clipManager(), clip});
    };

    for (int32_t trackIndex = 0; trackIndex < static_cast<int32_t>(tracks.size()); ++trackIndex)
        collectTrack(tracks[trackIndex]);
    collectTrack(appModel.getMasterTimelineTrack());

    std::unordered_map<ClipKey, uapmd::TimelinePosition, ClipKeyHash> resolvedPositions;
    std::unordered_set<ClipKey, ClipKeyHash> resolving;

    std::function<uapmd::TimelinePosition(const ClipKey&)> resolveClipPosition =
        [&](const ClipKey& key) -> uapmd::TimelinePosition {
            auto resolvedIt = resolvedPositions.find(key);
            if (resolvedIt != resolvedPositions.end())
                return resolvedIt->second;

            auto recordIt = clipRecords.find(key);
            if (recordIt == clipRecords.end())
                return {};

            const auto& clip = recordIt->second.clip;
            if (clip.anchorReferenceId.empty()) {
                resolvedPositions[key] = clip.anchorOffset;
                return clip.anchorOffset;
            }

            if (!resolving.insert(key).second) {
                resolvedPositions[key] = clip.anchorOffset;
                return clip.anchorOffset;
            }

            const ClipKey anchorKey{clip.anchorReferenceId};
            auto anchorIt = clipRecords.find(anchorKey);
            if (anchorIt == clipRecords.end()) {
                resolving.erase(key);
                resolvedPositions[key] = clip.anchorOffset;
                return clip.anchorOffset;
            }

            auto anchorPosition = resolveClipPosition(anchorKey);
            if (clip.anchorOrigin == uapmd::AnchorOrigin::End)
                anchorPosition.samples += anchorIt->second.clip.durationSamples;

            auto resolved = anchorPosition + clip.anchorOffset;
            resolvedPositions[key] = resolved;
            resolving.erase(key);
            return resolved;
        };

    for (const auto& [key, record] : clipRecords)
        record.clipManager->setClipPosition(record.clip.clipId, resolveClipPosition(key));
}

void TimelineEditor::addClipToTrack(int32_t trackIndex, const std::string& filepath) {
    auto handleFile = [this, trackIndex](const std::string& selectedFile) {
        std::filesystem::path path(selectedFile);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".midi2") {
            uapmd::TimelinePosition position;
            position.samples = 0;
            position.legacy_beats = 0.0;

            auto& appModel = uapmd::AppModel::instance();
            auto result = appModel.addClipToTrack(trackIndex, position, nullptr, selectedFile);

            if (!result.success) {
                platformError("Add MIDI Clip Failed",
                              "Could not add MIDI clip to track: " + result.error);
                return;
            }

            refreshSequenceEditorForTrack(trackIndex);
            return;
        }

        auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
        if (!reader) {
            platformError("Load Failed",
                          "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG");
            return;
        }

        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;

        auto& appModel = uapmd::AppModel::instance();
        auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);

        if (!result.success) {
            platformError("Add Clip Failed",
                          "Could not add clip to track: " + result.error);
            return;
        }

        refreshSequenceEditorForTrack(trackIndex);
    };

    if (!filepath.empty()) {
        handleFile(filepath);
        return;
    }

    std::vector<uapmd::DocumentFilter> filters{
        {"All Supported", {}, {"*.wav", "*.flac", "*.ogg", "*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"Audio Files",   {}, {"*.wav", "*.flac", "*.ogg"}},
        {"MIDI Files",    {}, {"*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"WAV Files",     {}, {"*.wav"}},
        {"FLAC Files",    {}, {"*.flac"}},
        {"OGG Files",     {}, {"*.ogg"}},
        {"All Files",     {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [handleFile = std::move(handleFile)](uapmd::DocumentPickResult result) mutable {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [handleFile = std::move(handleFile)](const std::filesystem::path& resolved) mutable {
                        handleFile(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Open Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::addClipToTrackAtPosition(int32_t trackIndex, const std::string& filepath, double positionSeconds) {
    auto handleFile = [this, trackIndex, positionSeconds](const std::string& selectedFile) {
        std::filesystem::path path(selectedFile);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto& appModel = uapmd::AppModel::instance();
        const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

        uapmd::TimelinePosition position;
        double clampedPositionSeconds = std::max(0.0, positionSeconds);
        position.samples = static_cast<int64_t>(std::llround(clampedPositionSeconds * sampleRate));
        position.legacy_beats = 0.0;

        if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".midi2") {
            auto result = appModel.addClipToTrack(trackIndex, position, nullptr, selectedFile);
            if (!result.success) {
                platformError("Add MIDI Clip Failed",
                              "Could not add MIDI clip to track: " + result.error);
                return;
            }
            refreshSequenceEditorForTrack(trackIndex);
            return;
        }

        auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
        if (!reader) {
            platformError("Load Failed",
                          "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG");
            return;
        }

        auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);
        if (!result.success) {
            platformError("Add Clip Failed",
                          "Could not add clip to track: " + result.error);
            return;
        }

        refreshSequenceEditorForTrack(trackIndex);
    };

    if (!filepath.empty()) {
        handleFile(filepath);
        return;
    }

    std::vector<uapmd::DocumentFilter> filters{
        {"All Supported", {}, {"*.wav", "*.flac", "*.ogg", "*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"Audio Files",   {}, {"*.wav", "*.flac", "*.ogg"}},
        {"MIDI Files",    {}, {"*.mid", "*.midi", "*.smf", "*.midi2"}},
        {"WAV Files",     {}, {"*.wav"}},
        {"FLAC Files",    {}, {"*.flac"}},
        {"OGG Files",     {}, {"*.ogg"}},
        {"All Files",     {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [handleFile = std::move(handleFile)](uapmd::DocumentPickResult result) mutable {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [handleFile = std::move(handleFile)](const std::filesystem::path& resolved) mutable {
                        handleFile(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Open Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    if (trackIndex == uapmd::kMasterTrackIndex && clipId == kMasterTrackClipId) {
        return;
    }
    appModel.removeClipFromTrack(trackIndex, clipId);
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::clearAllClipsFromTrack(int32_t trackIndex) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    tracks[trackIndex]->clipManager().clearAll();
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::updateClip(int32_t trackIndex, int32_t clipId, const std::string& anchorReferenceId, const std::string& origin, const std::string& position) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    double offsetSeconds = 0.0;
    try {
        std::string posStr = position;
        if (!posStr.empty() && posStr.back() == 's')
            posStr = posStr.substr(0, posStr.length() - 1);
        offsetSeconds = std::stod(posStr);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse position string: " << position << std::endl;
        return;
    }

    uapmd::AnchorOrigin anchorOrigin = uapmd::AnchorOrigin::Start;
    if (origin == "End")
        anchorOrigin = uapmd::AnchorOrigin::End;

    uapmd::TimelinePosition anchorOffset = uapmd::TimelinePosition::fromSeconds(offsetSeconds, appModel.sampleRate());
    auto* targetTrack = trackIndex == uapmd::kMasterTrackIndex
        ? appModel.getMasterTimelineTrack()
        : ((trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size())) ? tracks[trackIndex] : nullptr);
    if (!targetTrack)
        return;

    targetTrack->clipManager().setClipAnchor(clipId, anchorReferenceId, anchorOrigin, anchorOffset);
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::updateClipName(int32_t trackIndex, int32_t clipId, const std::string& name) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    tracks[trackIndex]->clipManager().setClipName(clipId, name);
    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::changeClipFile(int32_t trackIndex, int32_t clipId) {
    auto requestChange = [this, trackIndex, clipId](const std::string& selectedFile) {
        auto& appModel = uapmd::AppModel::instance();
        auto tracks = appModel.getTimelineTracks();

        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
            return;

        auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
        if (!reader) {
            platformError("Load Failed",
                          "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG");
            return;
        }

        auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
        if (!clip) {
            platformError("Error", "Could not find clip");
            return;
        }

        int32_t sourceNodeId = clip->sourceNodeInstanceId;

        auto sourceNode = std::make_unique<uapmd::AudioFileSourceNode>(
            sourceNodeId,
            std::move(reader),
            static_cast<double>(appModel.sampleRate())
        );

        int64_t durationSamples = sourceNode->totalLength();

        if (!tracks[trackIndex]->replaceClipSourceNode(clipId, std::move(sourceNode))) {
            platformError("Replace Failed", "Could not replace clip source node");
            return;
        }

        tracks[trackIndex]->clipManager().setClipFilepath(clipId, selectedFile);
        tracks[trackIndex]->clipManager().resizeClip(clipId, durationSamples);
        refreshSequenceEditorForTrack(trackIndex);
    };

    std::vector<uapmd::DocumentFilter> filters{
        {"Audio Files", {}, {"*.wav", "*.flac", "*.ogg"}},
        {"WAV Files",   {}, {"*.wav"}},
        {"FLAC Files",  {}, {"*.flac"}},
        {"OGG Files",   {}, {"*.ogg"}},
        {"All Files",   {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [requestChange = std::move(requestChange)](uapmd::DocumentPickResult result) mutable {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [requestChange = std::move(requestChange)](const std::filesystem::path& resolved) mutable {
                        requestChange(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Open Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::moveClipAbsolute(int32_t trackIndex, int32_t clipId, double seconds) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    double sr = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    uapmd::TimelinePosition newOffset = uapmd::TimelinePosition::fromSeconds(seconds, static_cast<int32_t>(sr));
    tracks[trackIndex]->clipManager().setClipAnchor(
        clipId,
        {},
        uapmd::AnchorOrigin::Start,
        newOffset);
    resolveAllClipAnchors();
    invalidateMasterTrackSnapshot();
    refreshAllSequenceEditorTracks();
}

void TimelineEditor::showMidiClipDump(int32_t trackIndex, int32_t clipId) {
    midiDumpWindow_.showClipDump(buildMidiClipDumpData(trackIndex, clipId));
}

void TimelineEditor::showPianoRoll(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;
    auto* track = tracks[trackIndex];
    if (!track)
        return;
    const auto* clipData = track->clipManager().getClip(clipId);
    if (!clipData)
        return;
    if (clipData->clipType != uapmd::ClipType::Midi)
        return;

    // createMidiClipPreview will determine duration from MIDI events;
    // pass 0.0 as fallback — it's only used when the clip has no events.
    const double fallbackDuration = 0.0;
    auto preview = createMidiClipPreview(trackIndex, *clipData, fallbackDuration);
    std::string clipName = clipData->name.empty()
        ? std::format("Clip {}", clipId) : clipData->name;
    pianoRollEditor_.showClip(trackIndex, clipId, clipName, std::move(preview));
}

void TimelineEditor::showMasterMetaDump() {
    midiDumpWindow_.showClipDump(buildMasterMetaDumpData());
}

MidiDumpWindow::ClipDumpData TimelineEditor::buildMidiClipDumpData(int32_t trackIndex, int32_t clipId) {
    MidiDumpWindow::ClipDumpData dump;
    dump.trackIndex = trackIndex;
    dump.clipId = clipId;

    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        dump.error = "Invalid track index";
        return dump;
    }

    auto* track = tracks[trackIndex];
    if (!track) {
        dump.error = "Track is unavailable";
        return dump;
    }

    auto* clip = track->clipManager().getClip(clipId);
    if (!clip) {
        dump.error = "Clip not found";
        return dump;
    }

    dump.clipName = clip->name.empty() ? std::format("Clip {}", clip->clipId) : clip->name;
    dump.filepath = clip->filepath;
    if (clip->clipType != uapmd::ClipType::Midi) {
        dump.error = "Selected clip is not a MIDI clip";
        return dump;
    }

    auto sourceNode = track->getSourceNode(clip->sourceNodeInstanceId);
    if (!sourceNode) {
        dump.error = "Source node not found";
        return dump;
    }

    auto* midiSourceNode = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
    if (!midiSourceNode) {
        dump.error = "Source node is not a MIDI clip source";
        return dump;
    }

    if (clip->filepath.empty()) {
        dump.fileLabel = "(in-memory)";
    } else {
        std::filesystem::path clipPath(clip->filepath);
        dump.fileLabel = clipPath.filename().string();
        if (dump.fileLabel.empty())
            dump.fileLabel = clip->filepath;
    }

    dump.tickResolution = midiSourceNode->tickResolution();
    dump.tempo = midiSourceNode->clipTempo();

    uapmd::MidiClipReader::ClipInfo clipInfo;
    clipInfo.success = true;
    clipInfo.ump_data = midiSourceNode->umpEvents();
    clipInfo.ump_tick_timestamps = midiSourceNode->eventTimestampsTicks();
    clipInfo.tick_resolution = dump.tickResolution;
    clipInfo.tempo = dump.tempo;

    dump.events = buildMidiDumpRows(clipInfo, dump.tickResolution, dump.tempo);
    dump.success = true;
    dump.error.clear();
    return dump;
}

MidiDumpWindow::ClipDumpData TimelineEditor::buildMasterMetaDumpData() {
    MidiDumpWindow::ClipDumpData dump;
    dump.trackIndex = -1;
    dump.clipId = -1;
    dump.isMasterTrack = true;
    dump.clipName = "Master Track Meta Events";
    dump.fileLabel = "Aggregated SMF meta events";

    std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot> snapshot = masterTrackSnapshot_;
    if (!snapshot) {
        snapshot = std::make_shared<uapmd::AppModel::MasterTrackSnapshot>(
            uapmd::AppModel::instance().buildMasterTrackSnapshot());
    }

    if (!snapshot || snapshot->empty()) {
        dump.success = false;
        dump.error = "No tempo or time signature events are available.";
        return dump;
    }

    struct MetaRow {
        double time{0.0};
        MidiDumpWindow::EventRow row;
    };

    std::vector<MetaRow> rows;
    rows.reserve(snapshot->tempoPoints.size() + snapshot->timeSignaturePoints.size());

    for (const auto& point : snapshot->tempoPoints) {
        if (point.bpm <= 0.0)
            continue;
        MidiDumpWindow::EventRow row;
        row.timeSeconds = point.timeSeconds;
        row.tickPosition = point.tickPosition;
        row.timeLabel = std::format("{:.6f}s [{}]", row.timeSeconds, row.tickPosition);

        double clampedBpm = std::clamp(point.bpm, 1.0, 1000.0);
        uint64_t usec = static_cast<uint64_t>(std::llround(60000000.0 / clampedBpm));
        if (usec > 0xFFFFFFu)
            usec = 0xFFFFFFu;

        uint8_t b0 = static_cast<uint8_t>((usec >> 16) & 0xFF);
        uint8_t b1 = static_cast<uint8_t>((usec >> 8) & 0xFF);
        uint8_t b2 = static_cast<uint8_t>(usec & 0xFF);
        row.hexBytes = std::format(
            "FF 51 03 {:02X} {:02X} {:02X}    (Tempo {:.2f} BPM)",
            b0, b1, b2, clampedBpm
        );
        rows.push_back(MetaRow{row.timeSeconds, row});
    }

    for (const auto& point : snapshot->timeSignaturePoints) {
        MidiDumpWindow::EventRow row;
        row.timeSeconds = point.timeSeconds;
        row.tickPosition = point.tickPosition;
        row.timeLabel = std::format("{:.6f}s [{}]", row.timeSeconds, row.tickPosition);

        uint8_t denominator = std::max<uint8_t>(1, point.signature.denominator);
        uint8_t exponent = 0;
        uint8_t denomValue = denominator;
        while (denomValue > 1 && exponent < 7) {
            denomValue >>= 1;
            ++exponent;
        }

        row.hexBytes = std::format(
            "FF 58 04 {:02X} {:02X} {:02X} {:02X}    (Time Sig {}/{} )",
            point.signature.numerator,
            exponent,
            point.signature.clocksPerClick,
            point.signature.thirtySecondsPerQuarter,
            point.signature.numerator,
            denominator
        );
        rows.push_back(MetaRow{row.timeSeconds, row});
    }

    std::sort(rows.begin(), rows.end(),
        [](const MetaRow& a, const MetaRow& b) {
            return a.time < b.time;
        });

    dump.events.reserve(rows.size());
    uint64_t prevTick = 0;
    for (auto& row : rows) {
        row.row.deltaTicks = row.row.tickPosition > prevTick ? row.row.tickPosition - prevTick : 0;
        prevTick = row.row.tickPosition;
        dump.events.push_back(std::move(row.row));
    }

    dump.success = true;
    dump.error.clear();
    return dump;
}

bool TimelineEditor::applyMidiClipEdits(const MidiDumpWindow::EditPayload& payload, std::string& error) {
    if (payload.trackIndex < 0) {
        error = "Editing master track meta events is not supported.";
        return false;
    }

    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (payload.trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[payload.trackIndex]) {
        error = "Track is unavailable.";
        return false;
    }

    auto& track = tracks[payload.trackIndex];
    auto* clip = track->clipManager().getClip(payload.clipId);
    if (!clip) {
        error = "Clip not found.";
        return false;
    }
    if (clip->clipType != uapmd::ClipType::Midi) {
        error = "Selected clip is not a MIDI clip.";
        return false;
    }

    auto sourceNode = track->getSourceNode(clip->sourceNodeInstanceId);
    auto midiNode = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(sourceNode);
    if (!midiNode) {
        error = "MIDI source node is not available.";
        return false;
    }

    std::vector<uapmd_ump_t> newEvents;
    std::vector<uint64_t> newTicks;
    size_t totalWords = 0;
    for (const auto& evt : payload.events) {
        if (evt.words.empty()) {
            error = "Event has empty data.";
            return false;
        }
        totalWords += evt.words.size();
    }
    newEvents.reserve(totalWords);
    newTicks.reserve(totalWords);

    uint64_t prevTick = 0;
    for (const auto& evt : payload.events) {
        if (!newEvents.empty() && evt.tickPosition < prevTick) {
            error = "Events must be ordered by tick.";
            return false;
        }
        prevTick = evt.tickPosition;
        for (auto word : evt.words) {
            newEvents.push_back(static_cast<uapmd_ump_t>(word));
            newTicks.push_back(evt.tickPosition);
        }
    }

    auto tempoChanges = midiNode->tempoChanges();
    auto timeSignatureChanges = midiNode->timeSignatureChanges();
    double clipTempo = midiNode->clipTempo();
    uint32_t tickResolution = clip->tickResolution > 0 ? clip->tickResolution : midiNode->tickResolution();
    auto newNode = std::make_unique<uapmd::MidiClipSourceNode>(
        midiNode->instanceId(),
        std::move(newEvents),
        std::move(newTicks),
        tickResolution,
        clipTempo,
        static_cast<double>(appModel.sampleRate()),
        tempoChanges,
        timeSignatureChanges
    );

    const int64_t newDuration = newNode->totalLength();
    if (!track->replaceClipSourceNode(clip->clipId, std::move(newNode))) {
        error = "Failed to replace MIDI clip data.";
        return false;
    }

    track->clipManager().resizeClip(clip->clipId, newDuration);
    refreshSequenceEditorForTrack(payload.trackIndex);
    return true;
}

bool TimelineEditor::applyPianoRollEdits(int32_t trackIndex, int32_t clipId,
                                          std::vector<uapmd_ump_t> newUmpEvents,
                                          std::vector<uint64_t>    newTickTimestamps,
                                          std::string&             error) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) ||
            !tracks[trackIndex]) {
        error = "Track unavailable.";
        return false;
    }
    auto& track = tracks[trackIndex];
    auto* clip = track->clipManager().getClip(clipId);
    if (!clip) {
        error = "Clip not found.";
        return false;
    }
    auto sourceNode = track->getSourceNode(clip->sourceNodeInstanceId);
    auto midiNode = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(sourceNode);
    if (!midiNode) {
        error = "MIDI source node unavailable.";
        return false;
    }
    uint32_t tickResolution = clip->tickResolution > 0
                              ? clip->tickResolution
                              : midiNode->tickResolution();
    auto newNode = std::make_unique<uapmd::MidiClipSourceNode>(
        midiNode->instanceId(),
        std::move(newUmpEvents),
        std::move(newTickTimestamps),
        tickResolution,
        midiNode->clipTempo(),
        static_cast<double>(appModel.sampleRate()),
        midiNode->tempoChanges(),
        midiNode->timeSignatureChanges()
    );
    const int64_t newDuration = newNode->totalLength();
    if (!track->replaceClipSourceNode(clipId, std::move(newNode))) {
        error = "Failed to replace MIDI clip data.";
        return false;
    }
    track->clipManager().resizeClip(clipId, newDuration);
    refreshSequenceEditorForTrack(trackIndex);
    return true;
}

// ── Plugin parameter query ────────────────────────────────────────────────────

std::vector<PianoRollEditor::PluginParamEntry>
TimelineEditor::getPluginParametersForTrack(int32_t trackIndex) const {
    std::vector<PianoRollEditor::PluginParamEntry> result;
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto tracksRef = sequencer.engine()->tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracksRef.size()) || !tracksRef[trackIndex])
        return result;
    for (int32_t instanceId : tracksRef[trackIndex]->orderedInstanceIds()) {
        auto* pal = sequencer.engine()->getPluginInstance(instanceId);
        if (!pal) continue;
        PianoRollEditor::PluginParamEntry entry;
        entry.instanceId = instanceId;
        entry.pluginName = pal->displayName();
        entry.group = tracksRef[trackIndex]->getInstanceGroup(instanceId);
        for (const auto& p : pal->parameterMetadataList()) {
            if (!p.automatable) continue;
            if (p.index >= 16384u) continue; // exceeds 14-bit addressable range
            PianoRollEditor::PluginParamEntry::Param param;
            param.nrpnIndex = static_cast<uint16_t>(
                ((p.index / 0x80u) << 7u) | (p.index % 0x80u));
            param.path = p.path;
            param.name = p.name;
            entry.params.push_back(std::move(param));
        }
        if (!entry.params.empty())
            result.push_back(std::move(entry));
    }
    return result;
}

// ── Per-type clip import helpers ──────────────────────────────────────────────

void TimelineEditor::addBlankMidi2ClipToTrack(int32_t trackIndex) {
    uapmd::TimelinePosition position;
    position.samples      = 0;
    position.legacy_beats = 0.0;
    auto& appModel = uapmd::AppModel::instance();
    auto result = appModel.addMidiClipToTrack(
        trackIndex, position,
        {},        // empty UMP events
        {},        // empty tick timestamps
        480,       // standard PPQ
        120.0,     // default BPM
        {},        // no tempo changes
        {},        // no time-signature changes
        "New Clip"
    );
    if (!result.success)
        platformError("New Clip Failed", "Could not create blank MIDI clip: " + result.error);
    else
        refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addBlankMidi2ClipToTrackAtPosition(int32_t trackIndex, double positionSeconds) {
    auto& appModel = uapmd::AppModel::instance();
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    uapmd::TimelinePosition position;
    position.samples      = static_cast<int64_t>(std::llround(std::max(0.0, positionSeconds) * sampleRate));
    position.legacy_beats = 0.0;
    auto result = appModel.addMidiClipToTrack(
        trackIndex, position,
        {},        // empty UMP events
        {},        // empty tick timestamps
        480,       // standard PPQ
        120.0,     // default BPM
        {},        // no tempo changes
        {},        // no time-signature changes
        "New Clip"
    );
    if (!result.success)
        platformError("New Clip Failed", "Could not create blank MIDI clip: " + result.error);
    else
        refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addAudioClipToTrack(int32_t trackIndex) {
    std::vector<uapmd::DocumentFilter> filters{
        {"Audio Files", {}, {"*.wav", "*.flac", "*.ogg"}},
        {"WAV Files",   {}, {"*.wav"}},
        {"FLAC Files",  {}, {"*.flac"}},
        {"OGG Files",   {}, {"*.ogg"}},
        {"All Files",   {}, {"*"}}
    };
    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters, false,
            [this, trackIndex](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty()) return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this, trackIndex](const std::filesystem::path& resolved) {
                        auto reader = uapmd::createAudioFileReaderFromPath(resolved.string());
                        if (!reader) {
                            platformError("Load Failed",
                                          "Could not load audio file: " + resolved.string());
                            return;
                        }
                        uapmd::TimelinePosition pos;
                        pos.samples = 0;
                        pos.legacy_beats = 0.0;
                        auto r = uapmd::AppModel::instance().addClipToTrack(
                            trackIndex, pos, std::move(reader), resolved.string());
                        if (!r.success)
                            platformError("Add Clip Failed", r.error);
                        else
                            refreshSequenceEditorForTrack(trackIndex);
                    },
                    [](const std::string& error) { platformError("Open Failed", error); });
            }
        );
    }
}

void TimelineEditor::addSmfClipToTrack(int32_t trackIndex) {
    std::vector<uapmd::DocumentFilter> filters{
        {"SMF Files", {}, {"*.mid", "*.midi", "*.smf"}},
        {"All Files", {}, {"*"}}
    };
    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters, false,
            [this, trackIndex](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty()) return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this, trackIndex](const std::filesystem::path& resolved) {
                        uapmd::TimelinePosition pos;
                        pos.samples = 0;
                        pos.legacy_beats = 0.0;
                        auto r = uapmd::AppModel::instance().addClipToTrack(
                            trackIndex, pos, nullptr, resolved.string());
                        if (!r.success)
                            platformError("Add Clip Failed", r.error);
                        else
                            refreshSequenceEditorForTrack(trackIndex);
                    },
                    [](const std::string& error) { platformError("Open Failed", error); });
            }
        );
    }
}

void TimelineEditor::addSmf2ClipToTrack(int32_t trackIndex) {
    std::vector<uapmd::DocumentFilter> filters{
        {"MIDI 2.0 Files", {}, {"*.midi2"}},
        {"All Files",      {}, {"*"}}
    };
    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters, false,
            [this, trackIndex](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty()) return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this, trackIndex](const std::filesystem::path& resolved) {
                        uapmd::TimelinePosition pos;
                        pos.samples = 0;
                        pos.legacy_beats = 0.0;
                        auto r = uapmd::AppModel::instance().addClipToTrack(
                            trackIndex, pos, nullptr, resolved.string());
                        if (!r.success)
                            platformError("Add Clip Failed", r.error);
                        else
                            refreshSequenceEditorForTrack(trackIndex);
                    },
                    [](const std::string& error) { platformError("Open Failed", error); });
            }
        );
    }
}

void TimelineEditor::importMidiTracksWithPicker() {
    std::vector<uapmd::DocumentFilter> filters{
        {"MIDI Files", {}, {"*.mid", "*.midi", "*.smf"}},
        {"All Files",  {}, {"*"}}
    };

    if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [this](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this](const std::filesystem::path& resolved) {
                        importMidiTracks(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Import Failed", error);
                    });
            }
        );
    }
}

void TimelineEditor::importMidiTracks(const std::string& filepath) {
    auto importResult = uapmd::import::TrackImporter::importMidiFile(filepath);
    auto warnings = importResult.warnings;

    if (!importResult.success) {
        std::string message = importResult.error.empty()
            ? "Failed to import MIDI tracks."
            : importResult.error;
        if (!warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
        return;
    }

    auto& appModel = uapmd::AppModel::instance();
    size_t importedCount = 0;

    for (auto& track : importResult.tracks) {
        int32_t newTrackIndex = appModel.addTrack();
        if (newTrackIndex < 0) {
            warnings.push_back(std::format("{}: Failed to create track", track.clipName));
            continue;
        }

        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;

        auto clipResult = appModel.addMidiClipToTrack(
            newTrackIndex,
            position,
            std::move(track.umpEvents),
            std::move(track.umpTickTimestamps),
            track.tickResolution,
            track.detectedTempo,
            std::move(track.tempoChanges),
            std::move(track.timeSignatureChanges),
            track.clipName,
            track.needsFileSave
        );

        if (!clipResult.success) {
            warnings.push_back(std::format("{}: {}", track.clipName, clipResult.error));
            continue;
        }

        refreshSequenceEditorForTrack(newTrackIndex);
        ++importedCount;
    }

    for (auto& masterTrackClip : importResult.masterTrackClips) {
        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;
        auto masterClipResult = appModel.addMasterMidiClip(
            position,
            {},
            {},
            masterTrackClip.tickResolution,
            masterTrackClip.detectedTempo,
            std::move(masterTrackClip.tempoChanges),
            std::move(masterTrackClip.timeSignatureChanges),
            masterTrackClip.clipName
        );
        if (!masterClipResult.success)
            warnings.push_back(std::format("{}: {}", masterTrackClip.clipName, masterClipResult.error));
    }

    invalidateMasterTrackSnapshot();

    if (importedCount == 0 && importResult.masterTrackClips.empty()) {
        std::string message = "No MIDI tracks were imported.";
        if (!warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
    }
}

void TimelineEditor::applyAudioImportResult(uapmd::import::AudioImportResult result) {
    auto warnings = result.warnings;
    if (result.canceled)
        return;
    if (!result.success) {
        std::string message = result.error.empty()
            ? "Failed to import audio stems."
            : result.error;
        if (!warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
        return;
    }

    auto& appModel = uapmd::AppModel::instance();
    size_t importedCount = 0;

    for (const auto& stem : result.stems) {
        int32_t newTrackIndex = appModel.addTrack();
        if (newTrackIndex < 0) {
            warnings.push_back(std::format("{}: Failed to create track", stem.clipDisplayName));
            continue;
        }

        auto reader = uapmd::createAudioFileReaderFromPath(stem.filepath.string());
        if (!reader) {
            warnings.push_back(std::format("{}: Failed to open stem audio", stem.clipDisplayName));
            continue;
        }

        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;

        auto clipResult = appModel.addClipToTrack(
            newTrackIndex,
            position,
            std::move(reader),
            stem.filepath.string()
        );

        if (!clipResult.success) {
            warnings.push_back(std::format("{}: {}", stem.clipDisplayName, clipResult.error));
            continue;
        }

        auto tracks = appModel.getTimelineTracks();
        if (newTrackIndex >= 0 && newTrackIndex < static_cast<int32_t>(tracks.size())) {
            auto& clipManager = tracks[newTrackIndex]->clipManager();
            clipManager.setClipName(clipResult.clipId, stem.clipDisplayName);
            clipManager.setClipNeedsFileSave(clipResult.clipId, true);
        }

        refreshSequenceEditorForTrack(newTrackIndex);
        ++importedCount;
    }

    if (importedCount == 0) {
        std::string message = "No stems were imported.";
        if (!warnings.empty()) {
            message += "\n\nWarnings:\n";
            for (const auto& warning : warnings)
                message += warning + "\n";
        }
        platformError("Import Failed", message);
    }
}

}  // namespace uapmd::gui
