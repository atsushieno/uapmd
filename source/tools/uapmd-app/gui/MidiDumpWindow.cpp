#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <format>
#include <cmath>
#include <limits>
#include <string_view>

#include <umppi/umppi.hpp>

#include "MidiDumpWindow.hpp"
#include "FontIcons.hpp"
#include <imgui.h>

namespace uapmd::gui {

constexpr float kDefaultWindowWidth = 620.0f;
constexpr float kDefaultWindowHeight = 420.0f;
constexpr ImVec4 kErrorBgColor = ImVec4(0.45f, 0.1f, 0.1f, 1.0f);
constexpr ImVec4 kErrorTextColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kSuccessTextColor = ImVec4(0.5f, 0.85f, 0.5f, 1.0f);
constexpr uint32_t kMaxWordsPerMessage = 4;

struct LengthOption {
    const char* label;
    int denominator;
};

constexpr LengthOption kLengthOptions[] = {
    {"0", 0},
    {"1/1", 1},
    {"1/2", 2},
    {"1/3", 3},
    {"1/4", 4},
    {"1/6", 6},
    {"1/8", 8},
    {"1/16", 16},
    {"1/24", 24},
    {"1/32", 32},
    {"1/48", 48},
};

uint64_t ticksForFraction(uint32_t tickResolution, int denominator) {
    if (denominator <= 0)
        return 0;
    const uint32_t tpq = tickResolution > 0 ? tickResolution : 480u;
    const double wholeNoteTicks = static_cast<double>(tpq) * 4.0;
    uint64_t ticks = static_cast<uint64_t>(std::llround(wholeNoteTicks / static_cast<double>(denominator)));
    return ticks == 0 ? 1 : ticks;
}

std::string_view trimView(std::string_view text) {
    auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

template <size_t N>
void copyStringToBuffer(const std::string& value, std::array<char, N>& buffer) {
    if (value.size() >= buffer.size()) {
        std::snprintf(buffer.data(), buffer.size(), "%.*s", static_cast<int>(buffer.size()) - 1, value.c_str());
        return;
    }
    std::memset(buffer.data(), 0, buffer.size());
    std::memcpy(buffer.data(), value.data(), value.size());
}

template <size_t N>
void copyCStringToBuffer(const char* value, std::array<char, N>& buffer) {
    if (!value) {
        buffer[0] = '\0';
        return;
    }
    std::snprintf(buffer.data(), buffer.size(), "%s", value);
}

uint64_t defaultDeltaTicks(const MidiDumpWindow::WindowState& state);
void onLengthEdited(MidiDumpWindow::WindowState& state, size_t index);
void insertEventRow(MidiDumpWindow::WindowState& state, size_t index);

void syncLengthEditBuffer(MidiDumpWindow::WindowState::EditableRow& row) {
    if (!row.editingLength)
        copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
}

void syncHexEditBuffer(MidiDumpWindow::WindowState::EditableRow& row) {
    if (!row.editingHex)
        copyCStringToBuffer(row.hexBuffer.data(), row.hexEditBuffer);
}

double ticksToSeconds(uint64_t ticks, uint32_t tickResolution, double tempo) {
    const double safeTempo = tempo > 0.0 ? tempo : 120.0;
    const double safeTicks = tickResolution > 0 ? static_cast<double>(tickResolution) : 480.0;
    double beats = static_cast<double>(ticks) / safeTicks;
    return beats * (60.0 / safeTempo);
}

std::string buildTimeLabel(uint64_t ticks, uint32_t tickResolution, double tempo) {
    double seconds = ticksToSeconds(ticks, tickResolution, tempo);
    return std::format("{:.3f}s [{}]", seconds, ticks);
}

bool parseUint64(std::string_view text, uint64_t& value) {
    text = trimView(text);
    if (text.empty())
        return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    uint64_t parsed = 0;
    auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end)
        return false;
    value = parsed;
    return true;
}

bool parseTickLength(std::string_view text, uint64_t& ticksOut, std::string& error) {
    text = trimView(text);
    if (text.empty()) {
        error = "Length is required";
        return false;
    }
    if (!parseUint64(text, ticksOut)) {
        error = "Length must be an integer number of ticks";
        return false;
    }
    return true;
}

bool parseHexWords(std::string_view input, std::vector<uint32_t>& words, std::string& error) {
    std::string sanitized;
    sanitized.reserve(input.size());
    for (char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch)))
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            error = "Hex data must use 0-9 or A-F";
            return false;
        }
        sanitized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    if (sanitized.empty()) {
        error = "Enter hex bytes";
        return false;
    }
    if (sanitized.size() % 2 != 0) {
        error = "Hex bytes must be pairs";
        return false;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(sanitized.size() / 2);
    for (size_t i = 0; i < sanitized.size(); i += 2) {
        uint32_t value = 0;
        auto result = std::from_chars(sanitized.data() + i, sanitized.data() + i + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != sanitized.data() + i + 2) {
            error = "Invalid hex byte";
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }

    if (bytes.size() % 4 != 0) {
        error = "UMP messages must be 32-bit aligned";
        return false;
    }
    const size_t wordCount = bytes.size() / 4;
    if (wordCount == 0 || wordCount > kMaxWordsPerMessage) {
        error = "UMP messages must be 1-4 words";
        return false;
    }

    words.resize(wordCount);
    for (size_t i = 0; i < wordCount; ++i) {
        size_t offset = i * 4;
        words[i] = (static_cast<uint32_t>(bytes[offset]) << 24) |
                   (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
                   (static_cast<uint32_t>(bytes[offset + 3]));
    }

    uint8_t messageType = static_cast<uint8_t>((words[0] >> 28) & 0xF);
    int expectedWords = umppi::umpSizeInInts(messageType);
    if (expectedWords <= 0) {
        error = "Unknown UMP message type";
        return false;
    }
    if (words.size() != static_cast<size_t>(expectedWords)) {
        error = std::format("Message type expects {} words", expectedWords);
        return false;
    }

    return true;
}

void restoreLengthValidation(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    uint64_t dummy = 0;
    std::string error;
    if (parseTickLength(row.lengthBuffer.data(), dummy, error)) {
        row.lengthValid = true;
        row.lengthError.clear();
    } else {
        row.lengthValid = false;
        row.lengthError = error;
    }
}

void restoreHexValidation(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    std::string error;
    std::vector<uint32_t> words;
    if (parseHexWords(row.hexBuffer.data(), words, error)) {
        row.hexValid = true;
        row.hexError.clear();
        row.parsedWords = std::move(words);
    } else {
        row.hexValid = false;
        row.hexError = error;
    }
}

void startLengthEdit(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
    row.editingLength = true;
    row.focusLength = true;
}

void startHexEdit(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    copyCStringToBuffer(row.hexBuffer.data(), row.hexEditBuffer);
    row.editingHex = true;
    row.focusHex = true;
}

void cancelLengthEdit(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
    row.editingLength = false;
    row.focusLength = false;
    restoreLengthValidation(state, index);
}

void cancelHexEdit(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    copyCStringToBuffer(row.hexBuffer.data(), row.hexEditBuffer);
    row.editingHex = false;
    row.focusHex = false;
    restoreHexValidation(state, index);
}

bool commitLengthEdit(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return false;
    auto& row = state.rows[index];
    uint64_t parsed = 0;
    std::string error;
    if (!parseTickLength(row.lengthEditBuffer.data(), parsed, error)) {
        row.lengthError = error;
        return false;
    }
    copyCStringToBuffer(row.lengthEditBuffer.data(), row.lengthBuffer);
    row.lengthValid = true;
    row.lengthError.clear();
    row.editingLength = false;
    row.focusLength = false;
    syncLengthEditBuffer(row);
    onLengthEdited(state, index);
    return true;
}

bool commitHexEdit(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return false;
    auto& row = state.rows[index];
    std::string error;
    std::vector<uint32_t> words;
    if (!parseHexWords(row.hexEditBuffer.data(), words, error)) {
        row.hexError = error;
        return false;
    }
    copyCStringToBuffer(row.hexEditBuffer.data(), row.hexBuffer);
    row.parsedWords = std::move(words);
    row.hexValid = true;
    row.hexError.clear();
    row.editingHex = false;
    row.focusHex = false;
    syncHexEditBuffer(row);
    state.hasPendingChanges = true;
    return true;
}

uint64_t deltaTicks(const MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return 0;
    const uint64_t prev = (index == 0) ? 0 : state.rows[index - 1].tickPosition;
    const uint64_t current = state.rows[index].tickPosition;
    return current > prev ? current - prev : 0;
}

void refreshLengthLabel(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    uint64_t delta = deltaTicks(state, index);
    copyStringToBuffer(std::to_string(delta), state.rows[index].lengthBuffer);
    syncLengthEditBuffer(state.rows[index]);
}

void refreshTickLabel(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto text = std::to_string(state.rows[index].tickPosition);
    copyStringToBuffer(text, state.rows[index].tickBuffer);
}

void refreshTimeLabel(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    state.rows[index].cachedSeconds = ticksToSeconds(
        state.rows[index].tickPosition,
        state.data.tickResolution,
        state.data.tempo);
}

void rebuildEditableRows(MidiDumpWindow::WindowState& state) {
    const bool readOnly = state.data.isMasterTrack || state.data.trackIndex < 0;
    state.readOnly = readOnly;
    state.rows.clear();
    state.rows.reserve(state.data.events.size());

    for (size_t i = 0; i < state.data.events.size(); ++i) {
        const auto& evt = state.data.events[i];
        MidiDumpWindow::WindowState::EditableRow row;
        row.tickPosition = evt.tickPosition;
        row.cachedSeconds = evt.timeSeconds;
        copyStringToBuffer(std::to_string(evt.tickPosition), row.tickBuffer);
        uint64_t delta = evt.deltaTicks;
        if (delta == 0 && i > 0) {
            const auto prev = state.data.events[i - 1].tickPosition;
            delta = evt.tickPosition > prev ? evt.tickPosition - prev : 0;
        }
        copyStringToBuffer(std::to_string(delta), row.lengthBuffer);
        copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
        copyStringToBuffer(evt.hexBytes, row.hexBuffer);
        copyCStringToBuffer(row.hexBuffer.data(), row.hexEditBuffer);
        if (!readOnly) {
            std::string error;
            row.hexValid = parseHexWords(row.hexBuffer.data(), row.parsedWords, error);
            if (!row.hexValid)
                row.hexError = error;
        } else {
            row.hexValid = true;
            row.hexError.clear();
        }
        state.rows.push_back(std::move(row));
    }

    if (state.rows.empty()) {
        MidiDumpWindow::WindowState::EditableRow row;
        row.tickPosition = 0;
        row.cachedSeconds = 0.0;
        copyStringToBuffer("0", row.tickBuffer);
        copyStringToBuffer(std::to_string(defaultDeltaTicks(state)), row.lengthBuffer);
        copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
        row.hexBuffer[0] = '\0';
        row.hexEditBuffer[0] = '\0';
        row.hexValid = false;
        row.hexError = "Enter hex bytes";
        state.rows.push_back(std::move(row));
    }

    for (size_t i = 0; i < state.rows.size(); ++i) {
        refreshLengthLabel(state, i);
        refreshTimeLabel(state, i);
    }

    state.hasPendingChanges = false;
    state.statusMessage.clear();
    state.statusColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

bool hasValidationErrors(const MidiDumpWindow::WindowState& state) {
    for (const auto& row : state.rows) {
        if (!row.lengthValid || !row.hexValid)
            return true;
    }
    return false;
}

uint64_t defaultDeltaTicks(const MidiDumpWindow::WindowState& state) {
    uint32_t tpq = state.data.tickResolution > 0 ? state.data.tickResolution : 480u;
    return std::max<uint32_t>(1, tpq / 4);
}

void shiftRowsFrom(MidiDumpWindow::WindowState& state, size_t index, int64_t diff) {
    if (diff == 0)
        return;
    for (size_t i = index; i < state.rows.size(); ++i) {
        int64_t shifted = static_cast<int64_t>(state.rows[i].tickPosition) + diff;
        if (shifted < 0)
            shifted = 0;
        state.rows[i].tickPosition = static_cast<uint64_t>(shifted);
        refreshTickLabel(state, i);
        refreshTimeLabel(state, i);
        if (i > 0)
            refreshLengthLabel(state, i);
    }
}

void onLengthEdited(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    uint64_t parsedTicks = deltaTicks(state, index);
    std::string error;
    if (!parseTickLength(row.lengthBuffer.data(), parsedTicks, error)) {
        row.lengthValid = false;
        row.lengthError = error;
        return;
    }

    const uint64_t prevTick = (index == 0) ? 0 : state.rows[index - 1].tickPosition;
    const uint64_t newTick = prevTick + parsedTicks;
    const uint64_t oldTick = row.tickPosition;
    if (newTick == oldTick) {
        row.lengthValid = true;
        row.lengthError.clear();
        return;
    }

    int64_t diff = static_cast<int64_t>(newTick) - static_cast<int64_t>(oldTick);
    row.tickPosition = newTick;
    row.lengthValid = true;
    row.lengthError.clear();
    refreshTickLabel(state, index);
    refreshLengthLabel(state, index);
    refreshTimeLabel(state, index);
    shiftRowsFrom(state, index + 1, diff);
    state.hasPendingChanges = true;
}

void addEventRow(MidiDumpWindow::WindowState& state) {
    MidiDumpWindow::WindowState::EditableRow row;
    const uint64_t lastTick = state.rows.empty() ? 0 : state.rows.back().tickPosition;
    const uint64_t newTick = lastTick + defaultDeltaTicks(state);
    row.tickPosition = newTick;
    row.cachedSeconds = ticksToSeconds(newTick, state.data.tickResolution, state.data.tempo);
    copyStringToBuffer(std::to_string(newTick), row.tickBuffer);
    copyStringToBuffer(std::to_string(defaultDeltaTicks(state)), row.lengthBuffer);
    copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
    row.hexBuffer[0] = '\0';
    row.hexEditBuffer[0] = '\0';
    row.hexValid = false;
    row.hexError = "Enter hex bytes";
    state.rows.push_back(std::move(row));
    state.hasPendingChanges = true;
}

void insertEventRow(MidiDumpWindow::WindowState& state, size_t index) {
    MidiDumpWindow::WindowState::EditableRow row;
    uint64_t newTick = 0;
    if (index < state.rows.size()) {
        newTick = state.rows[index].tickPosition;
    } else if (!state.rows.empty()) {
        newTick = state.rows.back().tickPosition + defaultDeltaTicks(state);
    }
    row.tickPosition = newTick;
    row.cachedSeconds = ticksToSeconds(newTick, state.data.tickResolution, state.data.tempo);
    copyStringToBuffer(std::to_string(newTick), row.tickBuffer);
    copyStringToBuffer(std::to_string(defaultDeltaTicks(state)), row.lengthBuffer);
    copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
    row.hexBuffer[0] = '\0';
    row.hexEditBuffer[0] = '\0';
    row.hexValid = false;
    row.hexError = "Enter hex bytes";
    state.rows.insert(state.rows.begin() + static_cast<long>(index), std::move(row));
    for (size_t i = index; i < state.rows.size(); ++i) {
        refreshLengthLabel(state, i);
        refreshTickLabel(state, i);
        refreshTimeLabel(state, i);
    }
    state.hasPendingChanges = true;
}

void deleteEventRow(MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return;
    state.rows.erase(state.rows.begin() + static_cast<long>(index));
    if (state.rows.empty()) {
        MidiDumpWindow::WindowState::EditableRow row;
        row.tickPosition = 0;
        copyStringToBuffer("0", row.tickBuffer);
        copyStringToBuffer(std::to_string(defaultDeltaTicks(state)), row.lengthBuffer);
        copyCStringToBuffer(row.lengthBuffer.data(), row.lengthEditBuffer);
        row.hexBuffer[0] = '\0';
        row.hexEditBuffer[0] = '\0';
        row.hexValid = false;
        row.hexError = "Enter hex bytes";
        state.rows.push_back(std::move(row));
    }
    for (size_t i = index; i < state.rows.size(); ++i) {
        refreshLengthLabel(state, i);
        refreshTickLabel(state, i);
        refreshTimeLabel(state, i);
    }
    state.hasPendingChanges = true;
}

void applyLengthPreset(MidiDumpWindow::WindowState& state, size_t index, uint64_t ticks) {
    if (index >= state.rows.size())
        return;
    auto& row = state.rows[index];
    copyStringToBuffer(std::to_string(ticks), row.lengthBuffer);
    row.editingLength = false;
    row.focusLength = false;
    syncLengthEditBuffer(row);
    onLengthEdited(state, index);
}

std::string buildLengthDisplay(const MidiDumpWindow::WindowState& state, size_t index) {
    if (index >= state.rows.size())
        return {};
    uint64_t delta = deltaTicks(state, index);
    for (const auto& opt : kLengthOptions) {
        uint64_t presetTicks = ticksForFraction(state.data.tickResolution, opt.denominator);
        if (presetTicks == delta) {
            if (opt.denominator == 0)
                return "0";
            return std::format("{} ({})", delta, opt.label);
        }
    }
    return std::to_string(delta);
}

void MidiDumpWindow::showClipDump(ClipDumpData data) {
    if (!data.isMasterTrack && (data.trackIndex < 0 || data.clipId < 0)) {
        return;
    }

    const auto key = std::make_pair(data.trackIndex, data.clipId);
    auto& state = windows_[key];
    state.data = std::move(data);
    state.visible = true;
    rebuildEditableRows(state);
}

void MidiDumpWindow::closeClip(int32_t trackIndex, int32_t clipId) {
    windows_.erase(std::make_pair(trackIndex, clipId));
}

void MidiDumpWindow::render(const RenderContext& context) {
    std::vector<std::pair<int32_t, int32_t>> keys;
    keys.reserve(windows_.size());
    for (const auto& entry : windows_) {
        keys.push_back(entry.first);
    }

    for (const auto& key : keys) {
        auto it = windows_.find(key);
        if (it == windows_.end()) {
            continue;
        }
        if (!it->second.visible) {
            continue;
        }
        renderWindow(it->second, context);
    }
}

std::string MidiDumpWindow::buildWindowTitle(const ClipDumpData& data) {
    if (data.isMasterTrack) {
        return "MIDI Dump - Master Track###MidiDumpMaster";
    }

    return std::format(
        "MIDI Dump - Track {} Clip {}###MidiDump{}_{}",
        data.trackIndex + 1,
        data.clipId,
        data.trackIndex,
        data.clipId
    );
}

void MidiDumpWindow::renderWindow(WindowState& state, const RenderContext& context) {
    auto& dump = state.data;
    std::string windowTitle = buildWindowTitle(dump);
    bool windowOpen = state.visible;
    const std::string windowSizeId = std::format("MidiDumpWindow{}_{}", dump.trackIndex, dump.clipId);

    if (context.setNextChildWindowSize) {
        context.setNextChildWindowSize(
            windowSizeId,
            ImVec2(kDefaultWindowWidth * context.uiScale, kDefaultWindowHeight * context.uiScale)
        );
    }

    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_NoCollapse)) {
        if (context.updateChildWindowSizeState) {
            context.updateChildWindowSizeState(windowSizeId);
        }

        ImGui::TextUnformatted(dump.clipName.c_str());
        if (!dump.fileLabel.empty()) {
            ImGui::Text("File: %s", dump.fileLabel.c_str());
        }

        if (dump.isMasterTrack) {
            ImGui::Text("Meta events: %zu", dump.events.size());
        } else {
            ImGui::Text("Tempo: %.2f BPM | PPQ: %u", dump.tempo, dump.tickResolution);
        }

        const bool hasApplyCallback = static_cast<bool>(context.applyEdits);
        const bool editingEnabled = !state.readOnly && hasApplyCallback;

        ImGui::Separator();

        if (!dump.success) {
            ImGui::TextColored(kErrorTextColor, "Failed to load clip: %s", dump.error.c_str());
            ImGui::End();
            state.visible = windowOpen;
            return;
        }

        if (state.readOnly) {
            ImGui::TextDisabled("Editing is disabled for this event list.");
        }

        bool confirmDiscard = false;
        if (context.reloadClip) {
            if (editingEnabled) {
                ImGui::BeginDisabled(!state.hasPendingChanges);
                if (ImGui::Button("Discard Changes")) {
                    if (state.hasPendingChanges) {
                        ImGui::OpenPopup("ConfirmDiscard");
                    }
                }
                ImGui::EndDisabled();
            } else if (ImGui::Button("Refresh")) {
                confirmDiscard = true;
            }
        }

        if (ImGui::BeginPopupModal("ConfirmDiscard", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Discard all edits and reload the clip?");
            ImGui::Spacing();
            if (ImGui::Button("Discard", ImVec2(120 * context.uiScale, 0))) {
                confirmDiscard = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120 * context.uiScale, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (confirmDiscard && context.reloadClip) {
            auto refreshed = context.reloadClip(dump.trackIndex, dump.clipId);
            if (refreshed.trackIndex == dump.trackIndex && refreshed.clipId == dump.clipId && refreshed.success) {
                state.data = std::move(refreshed);
                rebuildEditableRows(state);
            } else if (!refreshed.success) {
                state.statusMessage = refreshed.error.empty() ? "Failed to reload clip" : refreshed.error;
                state.statusColor = kErrorTextColor;
            }
        }

        ImGui::SameLine();
        const bool disabledApply = !editingEnabled || hasValidationErrors(state) || !state.hasPendingChanges;
        ImGui::BeginDisabled(disabledApply);
        if (ImGui::Button("Apply Changes") && context.applyEdits) {
            EditPayload payload;
            payload.trackIndex = dump.trackIndex;
            payload.clipId = dump.clipId;
            payload.tickResolution = dump.tickResolution;
            payload.isMasterTrack = dump.isMasterTrack;
            payload.events.reserve(state.rows.size());
            for (const auto& row : state.rows) {
                payload.events.push_back({});
                payload.events.back().tickPosition = row.tickPosition;
                payload.events.back().words = row.parsedWords;
            }

            std::string applyError;
            if (context.applyEdits(payload, applyError)) {
                bool reloadSucceeded = true;
                if (context.reloadClip) {
                    auto refreshed = context.reloadClip(dump.trackIndex, dump.clipId);
                    if (refreshed.success) {
                        state.data = std::move(refreshed);
                        rebuildEditableRows(state);
                    } else {
                        reloadSucceeded = false;
                        state.statusMessage = refreshed.error.empty()
                            ? "Updated, but failed to reload clip."
                            : refreshed.error;
                        state.statusColor = kErrorTextColor;
                    }
                } else {
                    rebuildEditableRows(state);
                }

                if (reloadSucceeded) {
                    state.statusMessage = "MIDI clip updated.";
                    state.statusColor = kSuccessTextColor;
                    state.hasPendingChanges = false;
                }
            } else {
                state.statusMessage = applyError.empty() ? "Failed to update clip." : applyError;
                state.statusColor = kErrorTextColor;
            }
        }
        ImGui::EndDisabled();

        if (!state.rows.empty()) {
            const int columnCount = editingEnabled ? 4 : 3;
            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Hideable;

            if (ImGui::BeginTable("MidiDumpTable", columnCount, tableFlags, ImVec2(0, 0))) {
                if (editingEnabled) {
                    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f * context.uiScale);
                }
                ImGui::TableSetupColumn("Time [Tick]", ImGuiTableColumnFlags_WidthFixed, 150.0f * context.uiScale);
                ImGui::TableSetupColumn("Delta Time", ImGuiTableColumnFlags_WidthFixed, 105.0f * context.uiScale);
                ImGui::TableSetupColumn("Message Bytes", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();

                int pendingDelete = -1;
                int pendingInsert = -1;
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(state.rows.size()));
                while (clipper.Step()) {
                    for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex) {
                        auto& row = state.rows[static_cast<size_t>(rowIndex)];
                        ImGui::PushID(rowIndex);
                        ImGui::TableNextRow();

                        if (editingEnabled) {
                            ImGui::TableSetColumnIndex(0);
                            const std::string rowMenuId = std::format("RowActions##{}", rowIndex);
                            if (ImGui::Button(std::format("{}##RowActionBtn{}", icons::ContextMenu, rowIndex).c_str())) {
                                ImGui::OpenPopup(rowMenuId.c_str());
                            }
                            if (ImGui::BeginPopup(rowMenuId.c_str())) {
                                if (ImGui::MenuItem("Insert Event Before")) {
                                    pendingInsert = rowIndex;
                                    ImGui::CloseCurrentPopup();
                                }
                                if (ImGui::MenuItem("Delete Event")) {
                                    pendingDelete = rowIndex;
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }

                        const int timeColumnIndex = editingEnabled ? 1 : 0;
                        const int lengthColumnIndex = editingEnabled ? 2 : 1;
                        const int messageColumnIndex = editingEnabled ? 3 : 2;

                        // Time column (read-only)
                        ImGui::TableSetColumnIndex(timeColumnIndex);
                        ImGui::Text("%s (%.3fs)", row.tickBuffer.data(), row.cachedSeconds);

                        // Length column
                        ImGui::TableSetColumnIndex(lengthColumnIndex);
                        const ImGuiStyle& style = ImGui::GetStyle();
                        const float buttonWidth = ImGui::GetFrameHeight();
                        const float minInputWidth = 60.0f * context.uiScale;
                        float cellWidth = ImGui::GetContentRegionAvail().x;
                        float contentWidth = cellWidth - buttonWidth - style.ItemInnerSpacing.x;
                        if (contentWidth < minInputWidth)
                            contentWidth = minInputWidth;
                        const size_t lengthIndex = static_cast<size_t>(rowIndex);
                        const bool showLengthError = (!row.lengthError.empty() && row.editingLength) || !row.lengthValid;
                        if (editingEnabled && row.editingLength) {
                            if (showLengthError)
                                ImGui::PushStyleColor(ImGuiCol_FrameBg, kErrorBgColor);
                            if (row.focusLength) {
                                ImGui::SetKeyboardFocusHere();
                                row.focusLength = false;
                            }
                            ImGui::SetNextItemWidth(contentWidth);
                            ImGuiInputTextFlags lengthFlags = ImGuiInputTextFlags_AutoSelectAll |
                                                             ImGuiInputTextFlags_CharsDecimal |
                                                             ImGuiInputTextFlags_EnterReturnsTrue;
                            bool lengthInputChanged = ImGui::InputText("##LengthTicks", row.lengthEditBuffer.data(),
                                                                       row.lengthEditBuffer.size(), lengthFlags);
                            if (row.editingLength && lengthInputChanged && row.lengthValid)
                                row.lengthError.clear();
                            bool cancel = false;
                            bool commit = false;
                            if (ImGui::IsItemFocused()) {
                                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                                    cancel = true;
                                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                                    commit = true;
                                if (ImGui::IsKeyPressed(ImGuiKey_Tab))
                                    commit = true;
                            }
                            if (commit) {
                                if (!commitLengthEdit(state, lengthIndex))
                                    row.focusLength = true;
                            } else if (cancel) {
                                cancelLengthEdit(state, lengthIndex);
                            } else if (!ImGui::IsItemActive()) {
                                row.focusLength = true;
                            }
                            if (showLengthError)
                                ImGui::PopStyleColor();
                            if (showLengthError && ImGui::IsItemHovered() && !row.lengthError.empty()) {
                                ImGui::SetTooltip("%s", row.lengthError.c_str());
                            }
                        } else {
                            std::string lengthDisplay = buildLengthDisplay(state, lengthIndex);
                            const ImVec4 textColor = !row.lengthValid
                                ? kErrorTextColor
                                : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                            ImGuiSelectableFlags selectableFlags = editingEnabled
                                ? ImGuiSelectableFlags_AllowDoubleClick
                                : ImGuiSelectableFlags_None;
                            ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                            ImGui::Selectable(lengthDisplay.c_str(), false, selectableFlags, ImVec2(contentWidth, 0.0f));
                            ImGui::PopStyleColor();
                            if (editingEnabled && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                startLengthEdit(state, lengthIndex);
                            }
                            if (!row.lengthError.empty() && ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", row.lengthError.c_str());
                        }
                        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
                        ImGui::BeginDisabled(!editingEnabled);
                        const std::string comboId = std::format("LengthPresetCombo##{}", rowIndex);
                        ImGui::SetNextItemWidth(buttonWidth);
                        if (ImGui::BeginCombo(comboId.c_str(), icons::ContextMenu)) {
                            uint64_t currentDelta = deltaTicks(state, lengthIndex);
                            for (const auto& opt : kLengthOptions) {
                                uint64_t presetTicks = ticksForFraction(state.data.tickResolution, opt.denominator);
                                std::string itemLabel = opt.denominator == 0
                                    ? "0 ticks"
                                    : std::format("{} ({} ticks)", opt.label, presetTicks);
                                bool selected = currentDelta == presetTicks;
                                if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                                    applyLengthPreset(state, lengthIndex, presetTicks);
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndDisabled();

                        // Message bytes column
                        ImGui::TableSetColumnIndex(messageColumnIndex);
                        float messageWidth = ImGui::GetContentRegionAvail().x;
                        if (messageWidth < 200.0f * context.uiScale)
                            messageWidth = 200.0f * context.uiScale;
                        const bool showHexError = (!row.hexError.empty() && row.editingHex) || !row.hexValid;
                        if (editingEnabled && row.editingHex) {
                            if (showHexError)
                                ImGui::PushStyleColor(ImGuiCol_FrameBg, kErrorBgColor);
                            if (row.focusHex) {
                                ImGui::SetKeyboardFocusHere();
                                row.focusHex = false;
                            }
                            ImGui::SetNextItemWidth(messageWidth);
                            ImGuiInputTextFlags hexFlags = ImGuiInputTextFlags_AutoSelectAll |
                                                           ImGuiInputTextFlags_CharsUppercase |
                                                           ImGuiInputTextFlags_EnterReturnsTrue;
                            bool hexInputChanged = ImGui::InputText("##Hex", row.hexEditBuffer.data(),
                                                                    row.hexEditBuffer.size(), hexFlags);
                            if (row.editingHex && hexInputChanged && row.hexValid)
                                row.hexError.clear();
                            bool cancel = false;
                            bool commit = false;
                            if (ImGui::IsItemFocused()) {
                                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                                    cancel = true;
                                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                                    commit = true;
                                if (ImGui::IsKeyPressed(ImGuiKey_Tab))
                                    commit = true;
                            }
                            if (commit) {
                                if (!commitHexEdit(state, static_cast<size_t>(rowIndex)))
                                    row.focusHex = true;
                            } else if (cancel) {
                                cancelHexEdit(state, static_cast<size_t>(rowIndex));
                            } else if (!ImGui::IsItemActive()) {
                                row.focusHex = true;
                            }
                            if (showHexError)
                                ImGui::PopStyleColor();
                            if (showHexError && ImGui::IsItemHovered() && !row.hexError.empty()) {
                                ImGui::SetTooltip("%s", row.hexError.c_str());
                            }
                        } else {
                            const ImVec4 textColor = !row.hexValid
                                ? kErrorTextColor
                                : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                            ImGuiSelectableFlags selectableFlags = editingEnabled
                                ? ImGuiSelectableFlags_AllowDoubleClick
                                : ImGuiSelectableFlags_None;
                            ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                            ImGui::Selectable(row.hexBuffer.data(), false, selectableFlags, ImVec2(messageWidth, 0.0f));
                            ImGui::PopStyleColor();
                            if (editingEnabled && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                startHexEdit(state, static_cast<size_t>(rowIndex));
                            }
                        }
                        if (showHexError && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", row.hexError.c_str());
                        }

                        ImGui::PopID();
                    }
                }

                ImGui::EndTable();

                if (pendingInsert >= 0) {
                    insertEventRow(state, static_cast<size_t>(pendingInsert));
                }
                if (pendingDelete >= 0) {
                    deleteEventRow(state, static_cast<size_t>(pendingDelete));
                }
            }
        } else {
            ImGui::TextUnformatted("No MIDI events available in this clip.");
        }

        if (!state.statusMessage.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(state.statusColor, "%s", state.statusMessage.c_str());
        }
    }
    ImGui::End();
    state.visible = windowOpen;
}

} // namespace uapmd::gui
