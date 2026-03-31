#include "AudioEventListEditor.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <set>
#include <string_view>

namespace uapmd::gui {

namespace {

constexpr float kDefaultWindowWidth = 680.0f;
constexpr float kDefaultWindowHeight = 520.0f;
constexpr int32_t kMasterMarkerTrackIndex = std::numeric_limits<int32_t>::min();
constexpr ImVec4 kErrorTextColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kSuccessTextColor = ImVec4(0.5f, 0.85f, 0.5f, 1.0f);
constexpr ImVec4 kHintTextColor = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
constexpr const char* kManualReferenceSelection = "(manual)";

std::string_view trimView(std::string_view text) {
    auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

template <size_t N>
void copyStringToBuffer(const std::string& value, std::array<char, N>& buffer) {
    std::memset(buffer.data(), 0, buffer.size());
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
}

std::string trimmedString(const char* text) {
    return std::string(trimView(text ? std::string_view(text) : std::string_view{}));
}

std::string fallbackMarkerLabel(std::string_view markerId, size_t index) {
    if (!markerId.empty())
        return std::string(markerId);
    return std::format("Marker {}", index + 1);
}

std::string generatedMarkerId(size_t index) {
    return std::format("marker_{}", index + 1);
}

std::string markerDisplayLabel(const AudioEventListEditor::MarkerRow& row, size_t index) {
    const std::string name = trimmedString(row.name.data());
    if (!name.empty())
        return name;
    return fallbackMarkerLabel(trimView(row.markerId.data()), index);
}

const AudioEventListEditor::MarkerRow* findMarkerRow(
    const std::vector<AudioEventListEditor::MarkerRow>& markerRows,
    std::string_view markerId
) {
    if (markerId.empty())
        return nullptr;
    auto it = std::find_if(markerRows.begin(), markerRows.end(), [markerId](const auto& row) {
        return trimView(row.markerId.data()) == markerId;
    });
    return it == markerRows.end() ? nullptr : &(*it);
}

bool warpMatchesOption(const AudioEventListEditor::WarpRow& row,
                       const AudioEventListEditor::ClipData::ReferencePointOption& option) {
    return row.referenceType == option.referenceType &&
           trimmedString(row.referenceClipId.data()) == option.referenceClipId &&
           trimmedString(row.referenceMarkerId.data()) == option.referenceMarkerId;
}

std::string referenceSelectorLabel(const AudioEventListEditor::WarpRow& row,
                                   const std::vector<AudioEventListEditor::ClipData::ReferencePointOption>& referenceOptions) {
    for (const auto& option : referenceOptions) {
        if (warpMatchesOption(row, option))
            return option.label;
    }
    if (row.referenceType == AudioWarpReferenceType::Manual)
        return kManualReferenceSelection;
    return "(unresolved)";
}

void syncWarpRowsToMarkers(AudioEventListEditor::WindowState& state) {
    for (auto& row : state.warpRows) {
        if (row.referenceType == AudioWarpReferenceType::ClipMarker &&
            trimmedString(row.referenceClipId.data()) == state.data.clipReferenceId) {
            const auto* marker = findMarkerRow(state.markerRows, trimView(row.referenceMarkerId.data()));
            if (marker) {
                row.clipPositionSamples = marker->clipPositionSamples;
                continue;
            }
        }
        for (const auto& option : state.data.externalReferenceOptions) {
            if (warpMatchesOption(row, option) && option.resolved) {
                row.clipPositionSamples = option.clipPositionSamples;
                break;
            }
        }
    }
}

std::vector<AudioEventListEditor::ClipData::ReferencePointOption> buildCurrentReferenceOptions(
    const AudioEventListEditor::WindowState& state,
    const std::vector<AudioEventListEditor::ClipData::ReferencePointOption>& externalReferenceOptions
) {
    std::vector<AudioEventListEditor::ClipData::ReferencePointOption> options;
    options.reserve(externalReferenceOptions.size() + state.markerRows.size() + 2);

    AudioEventListEditor::ClipData::ReferencePointOption clipStart;
    clipStart.label = "This Clip Start";
    clipStart.referenceType = AudioWarpReferenceType::ClipStart;
    clipStart.referenceClipId = state.data.clipReferenceId;
    clipStart.clipPositionSamples = 0;
    clipStart.resolved = true;
    options.push_back(std::move(clipStart));

    AudioEventListEditor::ClipData::ReferencePointOption clipEnd;
    clipEnd.label = "This Clip End";
    clipEnd.referenceType = AudioWarpReferenceType::ClipEnd;
    clipEnd.referenceClipId = state.data.clipReferenceId;
    clipEnd.clipPositionSamples = state.data.durationSamples;
    clipEnd.resolved = state.data.durationSamples >= 0;
    options.push_back(std::move(clipEnd));

    for (size_t i = 0; i < state.markerRows.size(); ++i) {
        const auto& markerRow = state.markerRows[i];
        AudioEventListEditor::ClipData::ReferencePointOption option;
        option.referenceType = AudioWarpReferenceType::ClipMarker;
        option.referenceClipId = state.data.clipReferenceId;
        option.referenceMarkerId = trimmedString(markerRow.markerId.data());
        option.label = std::format("This Clip Marker {}", markerDisplayLabel(markerRow, i));
        option.clipPositionSamples = markerRow.clipPositionSamples;
        option.resolved = !option.referenceMarkerId.empty() &&
            markerRow.clipPositionSamples >= 0 &&
            markerRow.clipPositionSamples <= state.data.durationSamples;
        if (!option.resolved)
            option.label += " (out of range)";
        options.push_back(std::move(option));
    }

    options.insert(options.end(), externalReferenceOptions.begin(), externalReferenceOptions.end());
    return options;
}

double samplesToSeconds(int64_t sampleCount, int32_t sampleRate) {
    if (sampleRate <= 0)
        return 0.0;
    return static_cast<double>(sampleCount) / static_cast<double>(sampleRate);
}

std::string sampleColumnLabel(std::string_view prefix, int32_t sampleRate) {
    if (sampleRate <= 0)
        return std::string(prefix);
    return std::format("{} @ {} Hz", prefix, sampleRate);
}

bool buildPayload(int32_t trackIndex,
                  int32_t clipId,
                  std::vector<AudioEventListEditor::MarkerRow>& markerRows,
                  const std::vector<AudioEventListEditor::WarpRow>& warpRows,
                  AudioEventListEditor::EditPayload& payload,
                  std::string& error) {
    payload.trackIndex = trackIndex;
    payload.clipId = clipId;
    payload.markers.clear();
    payload.audioWarps.clear();

    std::set<std::string> markerIds;
    payload.markers.reserve(markerRows.size());
    for (size_t i = 0; i < markerRows.size(); ++i) {
        auto& row = markerRows[i];
        std::string markerId = trimmedString(row.markerId.data());
        if (markerId.empty()) {
            markerId = generatedMarkerId(i);
            copyStringToBuffer(markerId, row.markerId);
        }
        if (markerId.empty()) {
            error = std::format("Marker {} requires a non-empty ID.", i + 1);
            return false;
        }
        if (row.clipPositionSamples < 0) {
            error = std::format("Marker {} has a negative clip position.", i + 1);
            return false;
        }

        if (!markerIds.insert(markerId).second) {
            error = std::format("Marker ID '{}' is duplicated.", markerId);
            return false;
        }

        uapmd::ClipMarker marker;
        marker.markerId = markerId;
        marker.clipPositionSamples = row.clipPositionSamples;
        marker.name = trimmedString(row.name.data());
        payload.markers.push_back(std::move(marker));
    }

    std::sort(payload.markers.begin(), payload.markers.end(), [](const auto& a, const auto& b) {
        if (a.clipPositionSamples != b.clipPositionSamples)
            return a.clipPositionSamples < b.clipPositionSamples;
        return a.markerId < b.markerId;
    });

    payload.audioWarps.reserve(warpRows.size());
    for (size_t i = 0; i < warpRows.size(); ++i) {
        const auto& row = warpRows[i];
        if (row.clipPositionSamples < 0) {
            error = std::format("Warp {} has a negative clip position.", i + 1);
            return false;
        }
        if (row.sourcePositionSamples < 0) {
            error = std::format("Warp {} has a negative source position.", i + 1);
            return false;
        }
        if (!std::isfinite(row.speedRatio) || row.speedRatio <= 0.0) {
            error = std::format("Warp {} must have a positive finite speed ratio.", i + 1);
            return false;
        }

        uapmd::AudioWarpPoint warp;
        warp.clipPositionSamples = row.clipPositionSamples;
        warp.sourcePositionSamples = row.sourcePositionSamples;
        warp.speedRatio = row.speedRatio;
        warp.referenceType = row.referenceType;
        warp.referenceClipId = trimmedString(row.referenceClipId.data());
        warp.referenceMarkerId = trimmedString(row.referenceMarkerId.data());

        if (warp.referenceType == AudioWarpReferenceType::ClipMarker &&
            warp.referenceClipId.empty() &&
            !warp.referenceMarkerId.empty() &&
            !markerIds.contains(warp.referenceMarkerId)) {
            error = std::format("Warp {} references unknown local marker ID '{}'.", i + 1, warp.referenceMarkerId);
            return false;
        }
        payload.audioWarps.push_back(std::move(warp));
    }

    std::sort(payload.audioWarps.begin(), payload.audioWarps.end(), [](const auto& a, const auto& b) {
        if (a.clipPositionSamples != b.clipPositionSamples)
            return a.clipPositionSamples < b.clipPositionSamples;
        if (a.sourcePositionSamples != b.sourcePositionSamples)
            return a.sourcePositionSamples < b.sourcePositionSamples;
        if (a.referenceType != b.referenceType)
            return static_cast<int>(a.referenceType) < static_cast<int>(b.referenceType);
        if (a.referenceClipId != b.referenceClipId)
            return a.referenceClipId < b.referenceClipId;
        return a.referenceMarkerId < b.referenceMarkerId;
    });

    return true;
}

} // namespace

void AudioEventListEditor::showClip(ClipData data) {
    WindowState state;
    state.data = std::move(data);
    state.visible = true;
    populateRows(state);
    windows_[{state.data.trackIndex, state.data.clipId}] = std::move(state);
}

void AudioEventListEditor::closeClip(int32_t trackIndex, int32_t clipId) {
    windows_.erase({trackIndex, clipId});
}

std::unordered_map<std::string, std::vector<uapmd::ClipMarker>> AudioEventListEditor::draftMarkersByClipReference() const {
    std::unordered_map<std::string, std::vector<uapmd::ClipMarker>> result;
    for (const auto& [_, window] : windows_) {
        if (window.data.clipReferenceId.empty() || window.data.markerOnly)
            continue;
        std::vector<uapmd::ClipMarker> markers;
        markers.reserve(window.markerRows.size());
        for (const auto& row : window.markerRows) {
            uapmd::ClipMarker marker;
            marker.markerId = trimmedString(row.markerId.data());
            marker.clipPositionSamples = row.clipPositionSamples;
            marker.name = trimmedString(row.name.data());
            markers.push_back(std::move(marker));
        }
        result[window.data.clipReferenceId] = std::move(markers);
    }
    return result;
}

std::vector<uapmd::ClipMarker> AudioEventListEditor::draftMasterMarkers() const {
    for (const auto& [key, window] : windows_) {
        if (window.data.markerOnly && key.first == kMasterMarkerTrackIndex) {
            std::vector<uapmd::ClipMarker> markers;
            markers.reserve(window.markerRows.size());
            for (const auto& row : window.markerRows) {
                uapmd::ClipMarker marker;
                marker.markerId = trimmedString(row.markerId.data());
                marker.clipPositionSamples = row.clipPositionSamples;
                marker.name = trimmedString(row.name.data());
                markers.push_back(std::move(marker));
            }
            return markers;
        }
    }
    return {};
}

void AudioEventListEditor::render(const RenderContext& context) {
    std::vector<std::pair<int32_t, int32_t>> keys;
    keys.reserve(windows_.size());
    for (const auto& [key, _] : windows_)
        keys.push_back(key);

    for (const auto& key : keys) {
        auto it = windows_.find(key);
        if (it == windows_.end() || !it->second.visible)
            continue;
        renderWindow(it->second, context);
    }
}

std::string AudioEventListEditor::buildWindowTitle(const ClipData& data) {
    const std::string clipName = data.clipName.empty()
        ? std::format("Clip {}", data.clipId)
        : data.clipName;
    const std::string scopeLabel = data.trackIndex == kMasterMarkerTrackIndex
        ? std::string("Master Track")
        : std::format("Track {}", data.trackIndex + 1);
    const std::string titlePrefix = data.markerOnly ? "Markers" : "Audio Events";
    return std::format("{} - {} / {}###AudioEvents{}_{}", titlePrefix, scopeLabel, clipName, data.trackIndex, data.clipId);
}

void AudioEventListEditor::populateRows(WindowState& state) {
    state.markerRows.clear();
    state.warpRows.clear();

    state.markerRows.reserve(state.data.markers.size());
    for (const auto& marker : state.data.markers) {
        MarkerRow row;
        copyStringToBuffer(marker.markerId.empty() ? generatedMarkerId(state.markerRows.size()) : marker.markerId, row.markerId);
        row.clipPositionSamples = marker.clipPositionSamples;
        copyStringToBuffer(marker.name, row.name);
        state.markerRows.push_back(std::move(row));
    }

    state.warpRows.reserve(state.data.audioWarps.size());
    for (const auto& warp : state.data.audioWarps) {
        WarpRow row;
        row.clipPositionSamples = warp.clipPositionSamples;
        row.sourcePositionSamples = warp.sourcePositionSamples;
        row.speedRatio = warp.speedRatio;
        row.referenceType = warp.referenceType;
        copyStringToBuffer(warp.referenceClipId, row.referenceClipId);
        copyStringToBuffer(warp.referenceMarkerId, row.referenceMarkerId);
        state.warpRows.push_back(std::move(row));
    }

    syncWarpRowsToMarkers(state);
}

void AudioEventListEditor::renderWindow(WindowState& state, const RenderContext& context) {
    std::string windowTitle = buildWindowTitle(state.data);
    bool windowOpen = state.visible;
    std::string windowSizeId = std::format("AudioEventListEditor{}_{}", state.data.trackIndex, state.data.clipId);

    if (context.setNextChildWindowSize) {
        context.setNextChildWindowSize(windowSizeId,
            ImVec2(kDefaultWindowWidth * context.uiScale, kDefaultWindowHeight * context.uiScale));
    }

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(520.0f * context.uiScale, 280.0f * context.uiScale),
        ImVec2(FLT_MAX, FLT_MAX)
    );

    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_None)) {
        std::vector<ClipData::ReferencePointOption> externalReferenceOptions = state.data.externalReferenceOptions;
        if (context.buildExternalReferenceOptions)
            externalReferenceOptions = context.buildExternalReferenceOptions(state.data.trackIndex, state.data.clipId);
        state.data.externalReferenceOptions = externalReferenceOptions;
        syncWarpRowsToMarkers(state);
        const auto referenceOptions = buildCurrentReferenceOptions(state, externalReferenceOptions);

        if (context.updateChildWindowSizeState)
            context.updateChildWindowSizeState(windowSizeId);

        if (!state.data.fileLabel.empty())
            ImGui::Text("File: %s", state.data.fileLabel.c_str());
        if (!state.statusMessage.empty())
            ImGui::TextColored(state.statusColor, "%s", state.statusMessage.c_str());
        if (!state.data.error.empty())
            ImGui::TextColored(kErrorTextColor, "%s", state.data.error.c_str());
        ImGui::TextColored(kHintTextColor,
            "Clip samples are timeline output positions. Source samples are positions inside the audio file after load-time resampling.");
        ImGui::TextColored(kHintTextColor,
            "Reference points can resolve from this clip, other clips, or master-track markers. Unresolved references are preserved and ignored for rendering.");
        ImGui::Spacing();

        if (ImGui::Button("Reload")) {
            if (context.reloadClip) {
                state.data = context.reloadClip(state.data.trackIndex, state.data.clipId);
                populateRows(state);
                state.statusMessage.clear();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            EditPayload payload;
            std::string error;
            if (!buildPayload(state.data.trackIndex, state.data.clipId, state.markerRows, state.warpRows, payload, error)) {
                state.statusMessage = std::move(error);
                state.statusColor = kErrorTextColor;
            } else if (!context.applyEdits) {
                state.statusMessage = "Apply callback is unavailable.";
                state.statusColor = kErrorTextColor;
            } else if (!context.applyEdits(payload, error)) {
                state.statusMessage = std::move(error);
                state.statusColor = kErrorTextColor;
            } else {
                state.data.markers = payload.markers;
                state.data.audioWarps = payload.audioWarps;
                state.statusMessage = "Applied audio marker and warp changes.";
                state.statusColor = kSuccessTextColor;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Markers", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Add Marker")) {
                MarkerRow row;
                copyStringToBuffer(std::format("marker_{}", state.markerRows.size() + 1), row.markerId);
                state.markerRows.push_back(row);
            }

            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
            const float markerHeight = std::max(120.0f * context.uiScale,
                ImGui::GetTextLineHeightWithSpacing() * 6.0f);
            if (ImGui::BeginTable("AudioMarkersTable", 3, flags, ImVec2(0, markerHeight))) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                const std::string markerClipColumn = sampleColumnLabel("Clip Sample", state.data.sampleRate);
                ImGui::TableSetupColumn(markerClipColumn.c_str(), ImGuiTableColumnFlags_WidthFixed, 130.0f * context.uiScale);
                ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < state.markerRows.size();) {
                    bool erase = false;
                    auto& row = state.markerRows[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputText("##marker_name", row.name.data(), row.name.size());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputScalar("##marker_clip_position", ImGuiDataType_S64, &row.clipPositionSamples);
                    if (state.data.sampleRate > 0)
                        ImGui::SetItemTooltip("%.6fs", samplesToSeconds(row.clipPositionSamples, state.data.sampleRate));

                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::Button("Delete"))
                        erase = true;

                    ImGui::PopID();
                    if (erase)
                        state.markerRows.erase(state.markerRows.begin() + static_cast<std::ptrdiff_t>(i));
                    else
                        ++i;
                }

                ImGui::EndTable();
            }
        }

        if (!state.data.markerOnly && ImGui::CollapsingHeader("Warps", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Add Warp")) {
                WarpRow row;
                state.warpRows.push_back(row);
            }

            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
            const float warpHeight = std::max(180.0f * context.uiScale,
                ImGui::GetTextLineHeightWithSpacing() * 8.0f);
            if (ImGui::BeginTable("AudioWarpsTable", 5, flags, ImVec2(0, warpHeight))) {
                ImGui::TableSetupColumn("Reference Point", ImGuiTableColumnFlags_WidthStretch);
                const std::string clipColumn = sampleColumnLabel("Clip Sample", state.data.sampleRate);
                const std::string sourceColumn = sampleColumnLabel("Source Sample", state.data.sampleRate);
                ImGui::TableSetupColumn(clipColumn.c_str(), ImGuiTableColumnFlags_WidthFixed, 130.0f * context.uiScale);
                ImGui::TableSetupColumn(sourceColumn.c_str(), ImGuiTableColumnFlags_WidthFixed, 130.0f * context.uiScale);
                ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthFixed, 100.0f * context.uiScale);
                ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < state.warpRows.size();) {
                    bool erase = false;
                    auto& row = state.warpRows[i];
                    ImGui::PushID(1000 + static_cast<int>(i));
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const std::string previewLabel = referenceSelectorLabel(row, referenceOptions);
                    if (ImGui::BeginCombo("##warp_reference", previewLabel.c_str())) {
                        const bool isManual = row.referenceType == AudioWarpReferenceType::Manual;
                        if (ImGui::Selectable(kManualReferenceSelection, isManual)) {
                            row.referenceType = AudioWarpReferenceType::Manual;
                            row.referenceClipId[0] = '\0';
                            row.referenceMarkerId[0] = '\0';
                        }

                        for (const auto& option : referenceOptions) {
                            const bool isSelected = warpMatchesOption(row, option);
                            if (ImGui::Selectable(option.label.c_str(), isSelected)) {
                                row.referenceType = option.referenceType;
                                copyStringToBuffer(option.referenceClipId, row.referenceClipId);
                                copyStringToBuffer(option.referenceMarkerId, row.referenceMarkerId);
                                if (option.resolved)
                                    row.clipPositionSamples = option.clipPositionSamples;
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::TableSetColumnIndex(1);
                    const bool hasResolvedReference = row.referenceType != AudioWarpReferenceType::Manual &&
                        std::any_of(referenceOptions.begin(), referenceOptions.end(), [&](const auto& option) {
                            return option.resolved && warpMatchesOption(row, option);
                        });
                    if (hasResolvedReference)
                        ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputScalar("##warp_clip_position", ImGuiDataType_S64, &row.clipPositionSamples);
                    if (hasResolvedReference)
                        ImGui::EndDisabled();
                    if (state.data.sampleRate > 0)
                        ImGui::SetItemTooltip("%.6fs", samplesToSeconds(row.clipPositionSamples, state.data.sampleRate));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputScalar("##warp_source_position", ImGuiDataType_S64, &row.sourcePositionSamples);
                    if (state.data.sampleRate > 0)
                        ImGui::SetItemTooltip("%.6fs", samplesToSeconds(row.sourcePositionSamples, state.data.sampleRate));

                    ImGui::TableSetColumnIndex(3);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputDouble("##warp_ratio", &row.speedRatio, 0.0, 0.0, "%.6f");

                    ImGui::TableSetColumnIndex(4);
                    if (ImGui::Button("Delete"))
                        erase = true;

                    ImGui::PopID();
                    if (erase)
                        state.warpRows.erase(state.warpRows.begin() + static_cast<std::ptrdiff_t>(i));
                    else
                        ++i;
                }

                ImGui::EndTable();
            }
        }
    }
    ImGui::End();

    if (!windowOpen)
        state.visible = false;
}

} // namespace uapmd::gui
