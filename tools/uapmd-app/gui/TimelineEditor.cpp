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
#include "../dialogs/FileDialogs.hpp"

#include <imgui.h>
#include <umppi/umppi.hpp>

#include "TimelineEditor.hpp"
#include "ClipPreview.hpp"
#include "FontIcons.hpp"
#include "../AppModel.hpp"
#include <uapmd-data/priv/project/SmfConverter.hpp>
#include <uapmd-data/priv/timeline/MidiClipSourceNode.hpp>

namespace uapmd::gui {

namespace {
constexpr int32_t kMasterTrackClipId = -1000;
constexpr double kDisplayDefaultBpm = 120.0;

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
        row.lengthBytes = byteLength;
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

    pluginSelector_.setOnScanPlugins([](bool forceRescan) {
        uapmd::AppModel::instance().performPluginScanning(forceRescan);
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
        .removeClip = [this](int32_t trackIndex, int32_t clipId) {
            removeClipFromTrack(trackIndex, clipId);
        },
        .clearAllClips = [this](int32_t trackIndex) {
            clearAllClipsFromTrack(trackIndex);
        },
        .updateClip = [this](int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position) {
            updateClip(trackIndex, clipId, anchorId, origin, position);
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
        .uiScale = uiScale,
    };
    midiDumpWindow_.render(midiDumpContext);
}

void TimelineEditor::renderPluginSelectorWindow(float uiScale) {
    if (!showPluginSelectorWindow_)
        return;

    const std::string windowId = "PluginSelector";
    if (setNextChildWindowSize_)
        setNextChildWindowSize_(windowId, ImVec2(520.0f, 560.0f));
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
        row.anchorClipId = -1;
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
            if (ImGui::MenuItem("New Clip"))
                context.addClip(trackIndex, "");
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

void TimelineEditor::handleTrackLayoutChange(const uapmd::AppModel::TrackLayoutChange& change) {
    switch (change.type) {
        case uapmd::AppModel::TrackLayoutChange::Type::Added:
            refreshSequenceEditorForTrack(change.trackIndex);
            break;
        case uapmd::AppModel::TrackLayoutChange::Type::Removed:
            sequenceEditor_.hideWindow(change.trackIndex);
            break;
        case uapmd::AppModel::TrackLayoutChange::Type::Cleared:
            sequenceEditor_.reset();
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
    std::unordered_map<int32_t, const uapmd::ClipData*> clipLookup;
    clipLookup.reserve(clips.size());
    for (auto& clip : clips)
        clipLookup[clip.clipId] = &clip;
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));

    for (const auto& clip : clips) {
        SequenceEditor::ClipRow row;
        row.clipId = clip.clipId;
        row.anchorClipId = clip.anchorClipId;

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

        auto absolutePosition = clip.getAbsolutePosition(clipLookup);
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
}

void TimelineEditor::addClipToTrack(int32_t trackIndex, const std::string& filepath) {
    std::string selectedFile = filepath;
    if (selectedFile.empty()) {
        auto selection = dialog::openFile(
            "Select Audio or MIDI File",
            ".",
            dialog::makeFilters(
                { "All Supported", "*.wav *.flac *.ogg *.mid *.midi *.smf *.midi2",
                  "Audio Files", "*.wav *.flac *.ogg",
                  "MIDI Files", "*.mid *.midi *.smf *.midi2",
                  "WAV Files", "*.wav",
                  "FLAC Files", "*.flac",
                  "OGG Files", "*.ogg",
                  "All Files", "*" })
        );

        if (selection.empty())
            return;

        selectedFile = selection[0].string();
    }

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
            dialog::showMessage("Add MIDI Clip Failed",
                        "Could not add MIDI clip to track: " + result.error,
                        dialog::MessageIcon::Error);
            return;
        }

        refreshSequenceEditorForTrack(trackIndex);
        return;
    }

    auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
    if (!reader) {
        dialog::showMessage("Load Failed",
                    "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG",
                    dialog::MessageIcon::Error);
        return;
    }

    uapmd::TimelinePosition position;
    position.samples = 0;
    position.legacy_beats = 0.0;

    auto& appModel = uapmd::AppModel::instance();
    auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);

    if (!result.success) {
        dialog::showMessage("Add Clip Failed",
                    "Could not add clip to track: " + result.error,
                    dialog::MessageIcon::Error);
        return;
    }

    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::addClipToTrackAtPosition(int32_t trackIndex, const std::string& filepath, double positionSeconds) {
    std::string selectedFile = filepath;
    if (selectedFile.empty()) {
        auto selection = dialog::openFile(
            "Select Audio or MIDI File",
            ".",
            dialog::makeFilters(
                { "All Supported", "*.wav *.flac *.ogg *.mid *.midi *.smf *.midi2",
                  "Audio Files", "*.wav *.flac *.ogg",
                  "MIDI Files", "*.mid *.midi *.smf *.midi2",
                  "WAV Files", "*.wav",
                  "FLAC Files", "*.flac",
                  "OGG Files", "*.ogg",
                  "All Files", "*" })
        );

        if (selection.empty())
            return;

        selectedFile = selection[0].string();
    }

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
            dialog::showMessage("Add MIDI Clip Failed",
                        "Could not add MIDI clip to track: " + result.error,
                        dialog::MessageIcon::Error);
            return;
        }

        refreshSequenceEditorForTrack(trackIndex);
        return;
    }

    auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
    if (!reader) {
        dialog::showMessage("Load Failed",
                    "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG",
                    dialog::MessageIcon::Error);
        return;
    }

    auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);

    if (!result.success) {
        dialog::showMessage("Add Clip Failed",
                    "Could not add clip to track: " + result.error,
                    dialog::MessageIcon::Error);
        return;
    }

    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    appModel.removeClipFromTrack(trackIndex, clipId);
    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::clearAllClipsFromTrack(int32_t trackIndex) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    tracks[trackIndex]->clipManager().clearAll();
    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::updateClip(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

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
    tracks[trackIndex]->clipManager().setClipAnchor(clipId, anchorId, anchorOrigin, anchorOffset);
    refreshSequenceEditorForTrack(trackIndex);
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
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    auto selection = dialog::openFile(
        "Select Audio File",
        ".",
        dialog::makeFilters(
            { "Audio Files", "*.wav *.flac *.ogg",
              "WAV Files", "*.wav",
              "FLAC Files", "*.flac",
              "OGG Files", "*.ogg",
              "All Files", "*" })
    );

    if (selection.empty())
        return;

    std::string selectedFile = selection[0].string();

    auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
    if (!reader) {
        dialog::showMessage("Load Failed",
                    "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG",
                    dialog::MessageIcon::Error);
        return;
    }

    auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
    if (!clip) {
        dialog::showMessage("Error",
                    "Could not find clip",
                    dialog::MessageIcon::Error);
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
        dialog::showMessage("Replace Failed",
                    "Could not replace clip source node",
                    dialog::MessageIcon::Error);
        return;
    }

    tracks[trackIndex]->clipManager().setClipFilepath(clipId, selectedFile);
    tracks[trackIndex]->clipManager().resizeClip(clipId, durationSamples);
    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::moveClipAbsolute(int32_t trackIndex, int32_t clipId, double seconds) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return;

    double sr = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    uapmd::TimelinePosition newOffset = uapmd::TimelinePosition::fromSeconds(seconds, static_cast<int32_t>(sr));
    tracks[trackIndex]->clipManager().setClipAnchor(clipId, -1, uapmd::AnchorOrigin::Start, newOffset);
    refreshSequenceEditorForTrack(trackIndex);
}

void TimelineEditor::showMidiClipDump(int32_t trackIndex, int32_t clipId) {
    midiDumpWindow_.showClipDump(buildMidiClipDumpData(trackIndex, clipId));
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
        row.lengthBytes = 6;

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
        row.lengthBytes = 7;

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
    for (auto& row : rows)
        dump.events.push_back(std::move(row.row));

    dump.success = true;
    dump.error.clear();
    return dump;
}

void TimelineEditor::importSmfTracks() {
    auto selection = dialog::openFile(
        "Import SMF Tracks",
        ".",
        dialog::makeFilters({ "MIDI Files", "*.mid *.midi *.smf",
          "All Files", "*" })
    );

    if (selection.empty())
        return;

    std::string selectedFile = selection[0].string();

    try {
        umppi::Midi1Music music = umppi::readMidi1File(selectedFile);

        if (music.tracks.empty()) {
            dialog::showMessage("Import Failed",
                        "The selected MIDI file contains no tracks.",
                        dialog::MessageIcon::Error);
            return;
        }

        auto& appModel = uapmd::AppModel::instance();
        std::filesystem::path smfPath(selectedFile);
        std::string baseFilename = smfPath.stem().string();

        std::vector<std::string> failures;

        for (size_t trackIdx = 0; trackIdx < music.tracks.size(); ++trackIdx) {
            auto convertResult = uapmd::SmfConverter::convertTrackToUmp(music, trackIdx);

            if (!convertResult.success) {
                failures.push_back(std::format("Track {}: {}", trackIdx + 1, convertResult.error));
                continue;
            }

            // Skip tracks with no MIDI events (empty tracks)
            if (convertResult.umpEvents.empty())
                continue;

            int32_t newTrackIndex = appModel.addTrack();
            if (newTrackIndex < 0) {
                failures.push_back(std::format("Track {}: Failed to create track", trackIdx + 1));
                continue;
            }

            uapmd::TimelinePosition position;
            position.samples = 0;
            position.legacy_beats = 0.0;

            std::string clipName = std::format("{} - Track {}", baseFilename, trackIdx + 1);

            auto clipResult = appModel.addMidiClipToTrack(
                newTrackIndex,
                position,
                std::move(convertResult.umpEvents),
                std::move(convertResult.umpEventTicksStamps),
                convertResult.tickResolution,
                convertResult.detectedTempo,
                std::move(convertResult.tempoChanges),
                std::move(convertResult.timeSignatureChanges),
                clipName
            );

            if (!clipResult.success) {
                failures.push_back(std::format("Track {}: {}", trackIdx + 1, clipResult.error));
                continue;
            }

            refreshSequenceEditorForTrack(newTrackIndex);
        }

        if (!failures.empty()) {
            std::string message = "The following tracks failed to import:\n\n";
            for (const auto& failure : failures)
                message += failure + "\n";

            dialog::showMessage("Import Warning",
                        message,
                        dialog::MessageIcon::Warning);
        }

    } catch (const std::exception& ex) {
        dialog::showMessage("Import Failed",
                    std::format("Exception during SMF import:\n{}", ex.what()),
                    dialog::MessageIcon::Error);
    }
}

}  // namespace uapmd::gui
