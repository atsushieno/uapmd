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

bool markerMatchesOption(const AudioEventListEditor::MarkerRow& row,
                         const AudioEventListEditor::ClipData::ReferencePointOption& option,
                         std::string_view clipReferenceId) {
    const std::string rowReferenceClipId = trimmedString(row.referenceClipId.data());
    const std::string effectiveReferenceClipId = rowReferenceClipId.empty() ? std::string(clipReferenceId) : rowReferenceClipId;
    return row.referenceType == option.referenceType &&
           effectiveReferenceClipId == option.referenceClipId &&
           trimmedString(row.referenceMarkerId.data()) == option.referenceMarkerId;
}

std::optional<int64_t> resolveMarkerRowClipPosition(
    const AudioEventListEditor::WindowState& state,
    const AudioEventListEditor::MarkerRow& row,
    const std::vector<AudioEventListEditor::ClipData::ReferencePointOption>& externalReferenceOptions,
    std::set<std::string>& resolvingMarkerIds
) {
    int64_t referencePosition = 0;
    switch (row.referenceType) {
        case AudioWarpReferenceType::Manual:
        case AudioWarpReferenceType::ClipStart:
            if (trimmedString(row.referenceClipId.data()).empty() ||
                trimmedString(row.referenceClipId.data()) == state.data.clipReferenceId) {
                referencePosition = 0;
                break;
            }
            [[fallthrough]];
        case AudioWarpReferenceType::ClipEnd:
        case AudioWarpReferenceType::MasterMarker: {
            const auto it = std::find_if(externalReferenceOptions.begin(), externalReferenceOptions.end(), [&](const auto& option) {
                return markerMatchesOption(row, option, state.data.clipReferenceId);
            });
            if (it == externalReferenceOptions.end() || !it->resolved)
                return std::nullopt;
            referencePosition = it->clipPositionSamples;
            break;
        }
        case AudioWarpReferenceType::ClipMarker: {
            const std::string referenceClipId = trimmedString(row.referenceClipId.data()).empty()
                ? state.data.clipReferenceId
                : trimmedString(row.referenceClipId.data());
            const std::string referenceMarkerId = trimmedString(row.referenceMarkerId.data());
            if (referenceClipId == state.data.clipReferenceId) {
                if (!resolvingMarkerIds.insert(referenceMarkerId).second)
                    return std::nullopt;
                const auto* marker = findMarkerRow(state.markerRows, referenceMarkerId);
                if (!marker) {
                    resolvingMarkerIds.erase(referenceMarkerId);
                    return std::nullopt;
                }
                auto resolved = resolveMarkerRowClipPosition(state, *marker, externalReferenceOptions, resolvingMarkerIds);
                resolvingMarkerIds.erase(referenceMarkerId);
                if (!resolved)
                    return std::nullopt;
                referencePosition = *resolved;
                break;
            }
            const auto it = std::find_if(externalReferenceOptions.begin(), externalReferenceOptions.end(), [&](const auto& option) {
                return markerMatchesOption(row, option, state.data.clipReferenceId);
            });
            if (it == externalReferenceOptions.end() || !it->resolved)
                return std::nullopt;
            referencePosition = it->clipPositionSamples;
            break;
        }
    }

    const int64_t clipPosition = referencePosition + row.clipPositionOffset;
    if (clipPosition < 0 || clipPosition > state.data.durationSamples)
        return std::nullopt;
    return clipPosition;
}

std::optional<int64_t> resolveMarkerRowClipPosition(
    const AudioEventListEditor::WindowState& state,
    const AudioEventListEditor::MarkerRow& row,
    const std::vector<AudioEventListEditor::ClipData::ReferencePointOption>& externalReferenceOptions
) {
    std::set<std::string> resolvingMarkerIds{trimmedString(row.markerId.data())};
    return resolveMarkerRowClipPosition(state, row, externalReferenceOptions, resolvingMarkerIds);
}

std::vector<AudioEventListEditor::ClipData::ReferencePointOption> buildCurrentReferenceOptions(
    const AudioEventListEditor::WindowState& state,
    const std::vector<AudioEventListEditor::ClipData::ReferencePointOption>& externalReferenceOptions
) {
    std::vector<AudioEventListEditor::ClipData::ReferencePointOption> options;
    options.reserve(externalReferenceOptions.size() + state.markerRows.size() + 2);

    options.push_back(AudioEventListEditor::ClipData::ReferencePointOption{
        .label = "This Clip Start",
        .referenceType = AudioWarpReferenceType::ClipStart,
        .referenceClipId = state.data.clipReferenceId,
        .clipPositionSamples = 0,
        .resolved = true
    });

    options.push_back(AudioEventListEditor::ClipData::ReferencePointOption{
        .label = "This Clip End",
        .referenceType = AudioWarpReferenceType::ClipEnd,
        .referenceClipId = state.data.clipReferenceId,
        .clipPositionSamples = state.data.durationSamples,
        .resolved = state.data.durationSamples >= 0
    });

    for (size_t i = 0; i < state.markerRows.size(); ++i) {
        const auto& markerRow = state.markerRows[i];
        AudioEventListEditor::ClipData::ReferencePointOption option;
        option.referenceType = AudioWarpReferenceType::ClipMarker;
        option.referenceClipId = state.data.clipReferenceId;
        option.referenceMarkerId = trimmedString(markerRow.markerId.data());
        option.label = std::format("This Clip Marker {}", markerDisplayLabel(markerRow, i));
        if (auto clipPosition = resolveMarkerRowClipPosition(state, markerRow, externalReferenceOptions)) {
            option.clipPositionSamples = *clipPosition;
            option.resolved = true;
        }
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
        if (row.clipPositionOffset < 0 && row.referenceType == AudioWarpReferenceType::ClipStart &&
            trimmedString(row.referenceClipId.data()).empty()) {
            error = std::format("Marker {} has a negative clip offset.", i + 1);
            return false;
        }

        if (!markerIds.insert(markerId).second) {
            error = std::format("Marker ID '{}' is duplicated.", markerId);
            return false;
        }

        uapmd::ClipMarker marker;
        marker.markerId = markerId;
        marker.clipPositionOffset = row.clipPositionOffset;
        marker.referenceType = row.referenceType;
        marker.referenceClipId = trimmedString(row.referenceClipId.data());
        marker.referenceMarkerId = trimmedString(row.referenceMarkerId.data());
        marker.name = trimmedString(row.name.data());
        payload.markers.push_back(std::move(marker));
    }

    payload.audioWarps.reserve(warpRows.size());
    for (size_t i = 0; i < warpRows.size(); ++i) {
        const auto& row = warpRows[i];
        if (row.clipPositionOffset < 0 && row.referenceType == AudioWarpReferenceType::ClipStart &&
            trimmedString(row.referenceClipId.data()).empty()) {
            error = std::format("Warp {} has a negative clip offset.", i + 1);
            return false;
        }
        if (!std::isfinite(row.speedRatio) || row.speedRatio <= 0.0) {
            error = std::format("Warp {} must have a positive finite speed ratio.", i + 1);
            return false;
        }

        uapmd::AudioWarpPoint warp;
        warp.clipPositionOffset = row.clipPositionOffset;
        warp.speedRatio = row.speedRatio;
        warp.referenceType = row.referenceType;
        warp.referenceClipId = trimmedString(row.referenceClipId.data());
        warp.referenceMarkerId = trimmedString(row.referenceMarkerId.data());

        if (warp.referenceType == AudioWarpReferenceType::ClipMarker &&
            (warp.referenceClipId.empty()) &&
            !warp.referenceMarkerId.empty() &&
            !markerIds.contains(warp.referenceMarkerId)) {
            error = std::format("Warp {} references unknown local marker ID '{}'.", i + 1, warp.referenceMarkerId);
            return false;
        }
        payload.audioWarps.push_back(std::move(warp));
    }

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
            marker.clipPositionOffset = row.clipPositionOffset;
            marker.referenceType = row.referenceType;
            marker.referenceClipId = trimmedString(row.referenceClipId.data());
            marker.referenceMarkerId = trimmedString(row.referenceMarkerId.data());
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
                marker.clipPositionOffset = row.clipPositionOffset;
                marker.referenceType = row.referenceType;
                marker.referenceClipId = trimmedString(row.referenceClipId.data());
                marker.referenceMarkerId = trimmedString(row.referenceMarkerId.data());
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
        row.clipPositionOffset = marker.clipPositionOffset;
        row.referenceType = marker.referenceType;
        copyStringToBuffer(marker.referenceClipId, row.referenceClipId);
        copyStringToBuffer(marker.referenceMarkerId, row.referenceMarkerId);
        copyStringToBuffer(marker.name, row.name);
        state.markerRows.push_back(std::move(row));
    }

    state.warpRows.reserve(state.data.audioWarps.size());
    for (const auto& warp : state.data.audioWarps) {
        WarpRow row;
        row.clipPositionOffset = warp.clipPositionOffset;
        row.speedRatio = warp.speedRatio;
        row.referenceType = warp.referenceType;
        copyStringToBuffer(warp.referenceClipId, row.referenceClipId);
        copyStringToBuffer(warp.referenceMarkerId, row.referenceMarkerId);
        state.warpRows.push_back(std::move(row));
    }

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
            "Markers and warps use offsets relative to the selected reference point.");
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
            if (ImGui::BeginTable("AudioMarkersTable", 4, flags, ImVec2(0, markerHeight))) {
                ImGui::TableSetupColumn("Reference Point", ImGuiTableColumnFlags_WidthStretch);
                const std::string markerClipColumn = sampleColumnLabel("Clip Offset", state.data.sampleRate);
                ImGui::TableSetupColumn(markerClipColumn.c_str(), ImGuiTableColumnFlags_WidthFixed, 130.0f * context.uiScale);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f * context.uiScale);
                ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < state.markerRows.size();) {
                    bool erase = false;
                    auto& row = state.markerRows[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string previewLabel = "(unresolved)";
                    for (const auto& option : referenceOptions) {
                        if (markerMatchesOption(row, option, state.data.clipReferenceId)) {
                            previewLabel = option.label;
                            break;
                        }
                    }
                    if (ImGui::BeginCombo("##marker_reference", previewLabel.c_str())) {
                        for (const auto& option : referenceOptions) {
                            const bool isSelected = markerMatchesOption(row, option, state.data.clipReferenceId);
                            if (ImGui::Selectable(option.label.c_str(), isSelected)) {
                                const auto previousType = row.referenceType;
                                const auto previousClipId = std::string(trimView(row.referenceClipId.data()));
                                const auto previousMarkerId = std::string(trimView(row.referenceMarkerId.data()));
                                row.referenceType = option.referenceType;
                                copyStringToBuffer(option.referenceClipId, row.referenceClipId);
                                copyStringToBuffer(option.referenceMarkerId, row.referenceMarkerId);
                                if (context.validateMarkerReference &&
                                    !context.validateMarkerReference(
                                        state.data.trackIndex,
                                        state.data.clipId,
                                        trimmedString(row.markerId.data()),
                                        row.referenceType,
                                        trimmedString(row.referenceClipId.data()),
                                        trimmedString(row.referenceMarkerId.data()))) {
                                    row.referenceType = previousType;
                                    copyStringToBuffer(previousClipId, row.referenceClipId);
                                    copyStringToBuffer(previousMarkerId, row.referenceMarkerId);
                                    state.statusMessage = "Recursive marker references are not allowed.";
                                    state.statusColor = kErrorTextColor;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputScalar("##marker_clip_offset", ImGuiDataType_S64, &row.clipPositionOffset);
                    if (auto resolvedClipPosition = resolveMarkerRowClipPosition(state, row, externalReferenceOptions);
                        resolvedClipPosition && state.data.sampleRate > 0) {
                        ImGui::SetItemTooltip("Resolved %.6fs", samplesToSeconds(*resolvedClipPosition, state.data.sampleRate));
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputText("##marker_name", row.name.data(), row.name.size());

                    ImGui::TableSetColumnIndex(3);
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
                const std::string sourceColumn = sampleColumnLabel("Offset", state.data.sampleRate);
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
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::TableSetColumnIndex(1);
                    auto resolvedReference = std::find_if(referenceOptions.begin(), referenceOptions.end(), [&](const auto& option) {
                        return option.resolved && warpMatchesOption(row, option);
                    });
                    int64_t displayedClipSample = row.clipPositionOffset;
                    if (resolvedReference != referenceOptions.end())
                        displayedClipSample = resolvedReference->clipPositionSamples + row.clipPositionOffset;
                    ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputScalar("##warp_clip_sample", ImGuiDataType_S64, &displayedClipSample);
                    ImGui::EndDisabled();
                    if (state.data.sampleRate > 0)
                        ImGui::SetItemTooltip("%.6fs", samplesToSeconds(displayedClipSample, state.data.sampleRate));

                ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputScalar("##warp_clip_offset", ImGuiDataType_S64, &row.clipPositionOffset);
                    if (state.data.sampleRate > 0)
                        ImGui::SetItemTooltip("%.6fs", samplesToSeconds(row.clipPositionOffset, state.data.sampleRate));

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
