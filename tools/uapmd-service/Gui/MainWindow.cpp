#include "MainWindow.hpp"

#include <algorithm>
#include <map>
#include <cctype>
#include <cstring>
#include <format>
#include <imgui.h>
#include <iostream>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"
#include <remidy/priv/event-loop.hpp>

#include "SharedTheme.hpp"

using uapmd::VirtualMidiDeviceController;

namespace uapmd::service::gui {

namespace {
std::string toLower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string bufferToString(const std::array<char, 128>& buffer) {
    return std::string(buffer.data(), strnlen(buffer.data(), buffer.size()));
}

std::string bufferToString(const std::array<char, 64>& buffer) {
    return std::string(buffer.data(), strnlen(buffer.data(), buffer.size()));
}

void copyToBuffer(std::string_view value, std::array<char, 128>& buffer) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::strncpy(buffer.data(), value.data(), buffer.size() - 1);
}

void copyToBuffer(std::string_view value, std::array<char, 64>& buffer) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::strncpy(buffer.data(), value.data(), buffer.size() - 1);
}

} // namespace

MainWindow::MainWindow(VirtualMidiDeviceController& controller, GuiDefaults defaults)
    : controller_(controller), defaults_(std::move(defaults)) {
    uapmd::gui::SetupImGuiStyle();

    copyToBuffer(defaults_.apiName, apiInput_);
    copyToBuffer(defaults_.deviceName, deviceNameInput_);
    pluginScanMessage_ = "Scanning plugins...";
    pendingDefaultDevice_ = !defaults_.pluginName.empty();
    startPluginScan(false);
}

MainWindow::~MainWindow() {
    if (scanningThread_.joinable()) {
        scanningThread_.join();
    }
    stopAllDevices();
}

void MainWindow::render() {
    if (!isOpen_) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("uapmd-service-main", &isOpen_, flags)) {
        ImGui::TextUnformatted("Plugin discovery");
        ImGui::Separator();
        {
            std::lock_guard lock(pluginMutex_);
            ImGui::TextWrapped("%s", pluginScanMessage_.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan##plugins") && !scanning_) {
            startPluginScan(true);
        }
        ImGui::Separator();

        renderPluginSelector();

        ImGui::Separator();
        renderDeviceManager();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void MainWindow::update() {
    if (!scanning_ && scanningThread_.joinable()) {
        scanningThread_.join();
    }

    if (!scanning_ && !defaultDeviceAttempted_) {
        attemptDefaultDeviceCreation();
    }
}

void MainWindow::shutdown() {
    isOpen_ = false;
    stopAllDevices();
    if (scanningThread_.joinable()) {
        scanningThread_.join();
    }
}

void MainWindow::startPluginScan(bool forceRescan) {
    if (scanning_) {
        return;
    }

    if (scanningThread_.joinable()) {
        scanningThread_.join();
    }

    scanning_ = true;
    pluginScanCompleted_ = false;
    {
        std::lock_guard lock(pluginMutex_);
        pluginScanMessage_ = forceRescan ? "Rescanning plugins..." : "Scanning plugins...";
        plugins_.clear();
        selectedPlugin_ = -1;
    }

    scanningThread_ = std::thread([this]() {
        remidy_tooling::PluginScanTool scanner{};
        int scanResult = 0;
        std::string errorMessage;
        try {
            scanResult = scanner.performPluginScanning();
        } catch (const std::exception& ex) {
            scanResult = -1;
            errorMessage = ex.what();
        }

        std::vector<PluginEntry> collected;
        if (scanResult == 0) {
            auto plugins = scanner.catalog.getPlugins();
            collected.reserve(plugins.size());
            for (auto* entry : plugins) {
                PluginEntry item{
                    .format = entry->format(),
                    .id = entry->pluginId(),
                    .name = entry->displayName(),
                    .vendor = entry->vendorName()
                };
                collected.push_back(std::move(item));
            }
            std::sort(collected.begin(), collected.end(),
                      [](const PluginEntry& a, const PluginEntry& b) { return a.name < b.name; });
        }

        finalizePluginScan(std::move(collected), scanResult, errorMessage);
    });
}

void MainWindow::finalizePluginScan(std::vector<PluginEntry>&& entries, int scanResult, const std::string& errorMessage) {
    {
        std::lock_guard lock(pluginMutex_);
        plugins_ = std::move(entries);
        pluginScanMessage_ = scanResult == 0
            ? std::format("Found {} plugins", plugins_.size())
            : std::format("Plugin scan failed: {}", errorMessage.empty() ? "unknown error" : errorMessage);
        if (selectedPlugin_ >= static_cast<int>(plugins_.size())) {
            selectedPlugin_ = plugins_.empty() ? -1 : 0;
        } else if (selectedPlugin_ < 0 && !plugins_.empty()) {
            selectedPlugin_ = 0;
        }
        if (selectedPlugin_ >= 0 && selectedPlugin_ < static_cast<int>(plugins_.size())) {
            selectedPluginFormat_ = plugins_[static_cast<size_t>(selectedPlugin_)].format;
            selectedPluginId_ = plugins_[static_cast<size_t>(selectedPlugin_)].id;
        } else {
            selectedPluginFormat_.clear();
            selectedPluginId_.clear();
        }
    }
    pluginScanCompleted_ = scanResult == 0;
    scanning_ = false;
}

std::vector<int> MainWindow::filteredPluginIndices(const std::vector<PluginEntry>& plugins) const {
    std::vector<int> indices;
    std::string filter = toLower(std::string(pluginFilter_.data(), strnlen(pluginFilter_.data(), pluginFilter_.size())));
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (filter.empty()) {
            indices.push_back(static_cast<int>(i));
            continue;
        }
        auto name = toLower(plugins[i].name);
        auto vendor = toLower(plugins[i].vendor);
        auto pluginId = toLower(plugins[i].id);
        if (name.find(filter) != std::string::npos || vendor.find(filter) != std::string::npos ||
            pluginId.find(filter) != std::string::npos) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
}

void MainWindow::renderPluginSelector() {
    std::vector<PluginEntry> pluginsCopy;
    {
        std::lock_guard lock(pluginMutex_);
        pluginsCopy = plugins_;
    }
    if (selectedPlugin_ < 0 || selectedPlugin_ >= static_cast<int>(pluginsCopy.size())) {
        selectedPlugin_ = -1;
        selectedPluginFormat_.clear();
        selectedPluginId_.clear();
    }

    ImGui::TextUnformatted("Available plugins");
    ImGui::InputText("Search", pluginFilter_.data(), pluginFilter_.size());

    auto visible = filteredPluginIndices(pluginsCopy);

    const float tableHeight = ImGui::GetTextLineHeightWithSpacing() * 10.0f;
    if (ImGui::BeginTable("PluginTable", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable,
                          ImVec2(-FLT_MIN, tableHeight))) {
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableHeadersRow();

        // Sort visible indices according to table sort specs
        if (!visible.empty()) {
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsCount > 0) {
                    auto cmp = [&](int lhsIdx, int rhsIdx) {
                        const auto& a = pluginsCopy[static_cast<size_t>(lhsIdx)];
                        const auto& b = pluginsCopy[static_cast<size_t>(rhsIdx)];
                        for (int n = 0; n < sort_specs->SpecsCount; n++) {
                            const ImGuiTableColumnSortSpecs* s = &sort_specs->Specs[n];
                            int delta = 0;
                            switch (s->ColumnIndex) {
                                case 0: delta = a.format.compare(b.format); break;
                                case 1: delta = a.name.compare(b.name); break;
                                case 2: delta = a.vendor.compare(b.vendor); break;
                                case 3: delta = a.id.compare(b.id); break;
                                default: break;
                            }
                            if (delta != 0) {
                                return (s->SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0) : (delta > 0);
                            }
                        }
                        // Tiebreaker for deterministic order
                        if (int t = a.name.compare(b.name); t != 0) return t < 0;
                        if (int t = a.vendor.compare(b.vendor); t != 0) return t < 0;
                        if (int t = a.id.compare(b.id); t != 0) return t < 0;
                        return a.format < b.format;
                    };
                    std::sort(visible.begin(), visible.end(), cmp);
                }
            }
        }

        for (int index : visible) {
            const auto& plugin = pluginsCopy[static_cast<size_t>(index)];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool selected = (selectedPluginFormat_ == plugin.format && selectedPluginId_ == plugin.id);
            std::string selectableId = std::format("##{}::{}::{}", plugin.format, plugin.id, index);
            if (ImGui::Selectable(selectableId.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedPlugin_ = index;
                selectedPluginFormat_ = plugin.format;
                selectedPluginId_ = plugin.id;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(plugin.format.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(plugin.name.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(plugin.vendor.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(plugin.id.c_str());
        }

        ImGui::EndTable();
    }

    // Build track destination options
    std::vector<std::string> labels;
    {
        trackOptions_.clear();
        if (auto* sequencer = controller_.sequencer()) {
            auto tracks = sequencer->getTrackInfos();
            for (const auto& track : tracks) {
                TrackDestinationOption option{
                    .deviceEntryId = -1,
                    .trackIndex = track.trackIndex,
                    .label = std::format("Track {}", track.trackIndex + 1)
                };
                trackOptions_.push_back(std::move(option));
            }
        }
        if (selectedTrackOption_ < 0 || selectedTrackOption_ > static_cast<int>(trackOptions_.size())) {
            selectedTrackOption_ = 0;
        }

        labels.reserve(trackOptions_.size() + 1);
        labels.emplace_back("New track (new UMP device)");
        for (const auto& option : trackOptions_) {
            labels.push_back(option.label);
        }
    }

    std::vector<const char*> labelPtrs;
    labelPtrs.reserve(labels.size());
    for (auto& label : labels) {
        labelPtrs.push_back(label.c_str());
    }

    bool canCreate = selectedPlugin_ >= 0 && selectedPlugin_ < static_cast<int>(pluginsCopy.size());
    if (!pluginScanCompleted_) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Create UMP Device") && canCreate) {
        const TrackDestinationOption* destination = nullptr;
        if (selectedTrackOption_ > 0 && static_cast<size_t>(selectedTrackOption_ - 1) < trackOptions_.size()) {
            destination = &trackOptions_[static_cast<size_t>(selectedTrackOption_ - 1)];
        }
        createDeviceForPlugin(static_cast<size_t>(selectedPlugin_), destination);
    }
    if (!pluginScanCompleted_) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("on");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250.0f);
    ImGui::Combo("##track_dest", &selectedTrackOption_, labelPtrs.data(), static_cast<int>(labelPtrs.size()));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Named as:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##device_name", deviceNameInput_.data(), deviceNameInput_.size());
    ImGui::SameLine();
    ImGui::TextUnformatted("API:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("##api", apiInput_.data(), apiInput_.size());
}

void MainWindow::renderDeviceManager() {
    ImGui::TextUnformatted("Audio tracks");
    std::vector<DeviceEntry> devicesCopy;
    {
        std::lock_guard lock(devicesMutex_);
        devicesCopy = devices_;
    }

    if (devicesCopy.empty()) {
        ImGui::TextDisabled("No virtual MIDI devices created yet.");
        return;
    }

    size_t removeIndex = static_cast<size_t>(-1);

    struct PluginRow {
        int32_t instanceId{-1};
        std::string pluginName{};
        std::string pluginFormat{};
        std::string pluginId{};
        std::string status{};
        bool pluginInstantiating{false};
        bool pluginHasError{false};
        int32_t trackIndex{-1};
        std::shared_ptr<DeviceState> deviceState{};
        size_t deviceIndex{static_cast<size_t>(-1)};
        std::string deviceLabel{};
        bool deviceRunning{false};
        bool deviceInstantiating{false};
        bool deviceHasError{false};
    };

    std::map<int32_t, std::vector<PluginRow>> rowsByTrack;
    std::vector<PluginRow> pendingRows;

    for (size_t deviceIdx = 0; deviceIdx < devicesCopy.size(); ++deviceIdx) {
        auto entry = devicesCopy[deviceIdx];
        auto state = entry.state;

        std::lock_guard guard(state->mutex);
        auto* sequencer = controller_.sequencer();
        const std::string deviceLabel = state->label.empty() ? std::format("Device {}", deviceIdx + 1) : state->label;

        if (state->pluginInstances.empty()) {
            pendingRows.push_back(PluginRow{
                -1,
                deviceLabel,
                "",
                "",
                state->statusMessage,
                state->instantiating,
                state->hasError,
                -1,
                state,
                deviceIdx,
                deviceLabel,
                state->running,
                state->instantiating,
                state->hasError
            });
            continue;
        }

        for (auto& [instanceId, pluginState] : state->pluginInstances) {
            PluginRow row{};
            row.instanceId = instanceId;
            row.pluginName = pluginState.pluginName;
            row.pluginFormat = pluginState.pluginFormat;
            row.pluginId = pluginState.pluginId;
            row.status = pluginState.statusMessage;
            row.pluginInstantiating = pluginState.instantiating;
            row.pluginHasError = pluginState.hasError;
            row.trackIndex = pluginState.trackIndex;
            row.deviceState = state;
            row.deviceIndex = deviceIdx;
            row.deviceLabel = deviceLabel;
            row.deviceRunning = state->running;
            row.deviceInstantiating = state->instantiating;
            row.deviceHasError = state->hasError;

            if (sequencer) {
                const auto trackIndex = sequencer->findTrackIndexForInstance(instanceId);
                if (trackIndex >= 0) {
                    pluginState.trackIndex = trackIndex;
                }
                row.trackIndex = pluginState.trackIndex;
            }

            if (row.trackIndex >= 0) {
                rowsByTrack[row.trackIndex].push_back(std::move(row));
            } else {
                pendingRows.push_back(std::move(row));
            }
        }
    }

    auto renderRow = [&](PluginRow& row) -> bool {
        ImGui::TableNextRow();

        auto deviceState = row.deviceState;
        bool deviceRunning = row.deviceRunning;
        bool deviceInstantiating = row.deviceInstantiating;
        bool deviceHasError = row.deviceHasError;
        std::string pluginStatus = row.status;
        bool pluginInstantiating = row.pluginInstantiating;
        bool pluginHasError = row.pluginHasError;

        if (deviceState) {
            std::lock_guard guard(deviceState->mutex);
            deviceRunning = deviceState->running;
            deviceInstantiating = deviceState->instantiating;
            deviceHasError = deviceState->hasError;
            if (row.instanceId >= 0) {
                if (auto* pluginPtr = findPluginInstance(*deviceState, row.instanceId)) {
                    pluginStatus = pluginPtr->statusMessage;
                    pluginInstantiating = pluginPtr->instantiating;
                    pluginHasError = pluginPtr->hasError;
                }
            }
        }

        auto* sequencerForRow = controller_.sequencer();
        const bool uiSupported = sequencerForRow && row.instanceId >= 0 && sequencerForRow->hasPluginUI(row.instanceId);
        const bool uiVisible = uiSupported && sequencerForRow->isPluginUIVisible(row.instanceId);

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(row.deviceLabel.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(row.pluginName.empty() ? "(pending)" : row.pluginName.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(row.pluginFormat.c_str());

        ImGui::TableSetColumnIndex(3);
        if (pluginInstantiating) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", pluginStatus.c_str());
        } else if (pluginHasError || deviceHasError) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", pluginStatus.c_str());
        } else {
            ImGui::TextUnformatted(pluginStatus.c_str());
        }

        ImGui::TableSetColumnIndex(4);
        bool removeTriggered = false;
        if (uiSupported) {
            const char* uiText = uiVisible ? "Hide UI" : "Show UI";
            std::string btnId = std::format("{}##{}::{}", uiText, row.deviceIndex, row.instanceId);
            if (ImGui::Button(btnId.c_str())) {
                if (uiVisible) {
                    hidePluginUIInstance(row.deviceState, row.instanceId);
                } else {
                    showPluginUIInstance(row.deviceState, row.instanceId);
                }
            }
            ImGui::SameLine();
        }

        if (deviceState && row.instanceId >= 0) {
            std::string runBtnId = std::format("{}##{}::{}::run", deviceRunning ? "Stop" : "Start", row.deviceIndex, row.instanceId);
            if (deviceInstantiating) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(runBtnId.c_str())) {
                std::lock_guard guard(deviceState->mutex);
                auto device = deviceState->device;
                if (device) {
                    if (deviceState->running) {
                        device->stop();
                        deviceState->running = false;
                        deviceState->statusMessage = "Stopped";
                    } else {
                        auto statusCode = device->start();
                        if (statusCode == 0) {
                            deviceState->running = true;
                            deviceState->statusMessage = "Running";
                            deviceState->hasError = false;
                        } else {
                            deviceState->statusMessage = std::format("Start failed (status {})", statusCode);
                            deviceState->hasError = true;
                        }
                    }
                }
            }
            if (deviceInstantiating) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
        }

        std::string removeBtnId = std::format("Remove##{}::{}::rm", row.deviceIndex, row.instanceId);
        if (ImGui::Button(removeBtnId.c_str())) {
            if (row.deviceState && row.instanceId >= 0) {
                hidePluginUIInstance(row.deviceState, row.instanceId);
            }
            removeIndex = row.deviceIndex;
            removeTriggered = true;
        }

        return removeTriggered;
    };

    auto* sequencer = controller_.sequencer();
    if (sequencer) {
        auto trackInfos = sequencer->getTrackInfos();
        bool exitLoops = false;
        for (const auto& track : trackInfos) {
            auto it = rowsByTrack.find(track.trackIndex);
            if (it == rowsByTrack.end()) {
                continue;
            }

            auto& rows = it->second;
            if (rows.empty()) {
                rowsByTrack.erase(it);
                continue;
            }

            std::unordered_map<int32_t, size_t> indexByInstance;
            indexByInstance.reserve(rows.size());
            for (size_t i = 0; i < rows.size(); ++i) {
                indexByInstance[rows[i].instanceId] = i;
            }
            std::vector<bool> displayed(rows.size(), false);

            bool tableOpen = false;

            auto beginTableIfNeeded = [&]() {
                if (!tableOpen) {
                    ImGui::Separator();
                    ImGui::Text("Track %d", track.trackIndex + 1);
                    ImGui::BeginTable(std::format("TrackPlugins-{}-{}", track.trackIndex, track.trackIndex).c_str(), 5,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame);
                    ImGui::TableSetupColumn("Device");
                    ImGui::TableSetupColumn("Plugin");
                    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Status");
                    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 240.0f);
                    ImGui::TableHeadersRow();
                    tableOpen = true;
                }
            };

            for (const auto& node : track.nodes) {
                auto idxIt = indexByInstance.find(node.instanceId);
                if (idxIt == indexByInstance.end()) {
                    continue;
                }
                beginTableIfNeeded();
                auto& row = rows[idxIt->second];
                displayed[idxIt->second] = true;
                if (renderRow(row)) {
                    exitLoops = true;
                    break;
                }
            }

            if (!exitLoops) {
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (displayed[i]) {
                        continue;
                    }
                    beginTableIfNeeded();
                    if (renderRow(rows[i])) {
                        exitLoops = true;
                        break;
                    }
                }
            }

            if (tableOpen) {
                ImGui::EndTable();
            }

            rowsByTrack.erase(it);

            if (exitLoops) {
                break;
            }
        }
    }

    for (auto& [_, rows] : rowsByTrack) {
        pendingRows.insert(pendingRows.end(), rows.begin(), rows.end());
    }
    rowsByTrack.clear();

    if (!pendingRows.empty() && removeIndex == static_cast<size_t>(-1)) {
        ImGui::Separator();
        ImGui::TextUnformatted("Pending plugins");
        if (ImGui::BeginTable("PendingPlugins", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Plugin");
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 240.0f);
            ImGui::TableHeadersRow();

            for (auto& row : pendingRows) {
                if (renderRow(row)) {
                    break;
                }
            }

            ImGui::EndTable();
        }
    }

    if (removeIndex != static_cast<size_t>(-1)) {
        removeDevice(removeIndex);
    }
}

void MainWindow::createDeviceForPlugin(size_t pluginIndex, const TrackDestinationOption* destination) {
    PluginEntry plugin;
    {
        std::lock_guard lock(pluginMutex_);
        if (pluginIndex >= plugins_.size()) {
            return;
        }
        plugin = plugins_[pluginIndex];
        selectedPluginFormat_ = plugin.format;
        selectedPluginId_ = plugin.id;
    }

    auto state = std::make_shared<DeviceState>();
    state->apiName = bufferToString(apiInput_);
    if (state->apiName.empty()) {
        state->apiName = defaults_.apiName;
    }
    state->label = bufferToString(deviceNameInput_);
    if (state->label.empty()) {
        state->label = std::format("{} [{}]", plugin.name, plugin.format);
    }
    state->statusMessage = destination ? std::format("Adding to track {}...", destination->trackIndex + 1)
                                       : "Instantiating plugin...";
    state->instantiating = true;

    std::string errorMessage;
    int32_t targetTrackIndex = destination ? destination->trackIndex : -1;
    auto device = controller_.createDevice(state->apiName, state->label, defaultManufacturer_, defaultVersion_,
                                           targetTrackIndex, plugin.format, plugin.id, errorMessage);

    if (!device) {
        std::lock_guard guard(state->mutex);
        state->statusMessage = "Plugin instantiation failed: " + errorMessage;
        state->hasError = true;
        state->instantiating = false;
        return;
    }

    state->device = device;

    int startStatus = device->start();
    {
        std::lock_guard guard(state->mutex);
        if (startStatus == 0) {
            state->running = true;
            state->statusMessage = "Running";
        } else {
            state->running = false;
            state->hasError = true;
            state->statusMessage = std::format("Failed to start device (status {})", startStatus);
            controller_.removeDevice(device->instanceId());
            state->device.reset();
            state->pluginInstances.erase(device->instanceId());
        }
        state->instantiating = false;

        if (startStatus == 0) {
            auto& node = state->pluginInstances[device->instanceId()];
            node.instanceId = device->instanceId();
            node.pluginName = plugin.name;
            node.pluginFormat = plugin.format;
            node.pluginId = plugin.id;
            node.statusMessage = std::format("Plugin ready (instance {})", node.instanceId);
            node.instantiating = false;
            node.hasError = false;
            node.trackIndex = device->trackIndex();
        }
    }

    {
        std::lock_guard lock(devicesMutex_);
        devices_.push_back(DeviceEntry{nextDeviceId_++, state});
    }
}

void MainWindow::removeDevice(size_t index) {
    std::shared_ptr<DeviceState> state;
    {
        std::lock_guard lock(devicesMutex_);
        if (index >= devices_.size()) {
            return;
        }
        state = devices_[index].state;
        devices_.erase(devices_.begin() + static_cast<long>(index));
    }

    if (state) {
        std::lock_guard guard(state->mutex);
        auto device = state->device;
        auto* sequencer = controller_.sequencer();
        for (auto& [instanceId, pluginState] : state->pluginInstances) {
            if (sequencer) {
                sequencer->destroyPluginUI(instanceId);
            }
            if (pluginState.pluginWindow) {
                pluginState.pluginWindow->show(false);
                pluginState.pluginWindow.reset();
            }
        }
        if (device) {
            device->stop();
            controller_.removeDevice(device->instanceId());
            state->device.reset();
        }
    }
}

void MainWindow::attemptDefaultDeviceCreation() {
    defaultDeviceAttempted_ = true;
    if (!pendingDefaultDevice_) {
        return;
    }

    std::vector<PluginEntry> pluginsCopy;
    {
        std::lock_guard lock(pluginMutex_);
        pluginsCopy = plugins_;
    }

    if (pluginsCopy.empty()) {
        return;
    }

    auto search = toLower(defaults_.pluginName);
    auto format = toLower(defaults_.formatName);

    auto matches = [&](const PluginEntry& entry, bool exact) {
        auto entryName = toLower(entry.name);
        auto entryId = toLower(entry.id);
        if (!format.empty() && toLower(entry.format) != format) {
            return false;
        }
        if (exact) {
            return entryName == search || entryId == search;
        }
        return entryName.find(search) != std::string::npos || entryId.find(search) != std::string::npos;
    };

    auto findMatch = [&](bool exact) -> std::optional<size_t> {
        for (size_t i = 0; i < pluginsCopy.size(); ++i) {
            if (matches(pluginsCopy[i], exact)) {
                return i;
            }
        }
        return std::nullopt;
    };

    std::optional<size_t> match = std::nullopt;
    if (!defaults_.pluginName.empty()) {
        match = findMatch(true);
        if (!match) {
            match = findMatch(false);
        }
    }

    if (match) {
        selectedPlugin_ = static_cast<int>(*match);
        selectedPluginFormat_ = pluginsCopy[*match].format;
        selectedPluginId_ = pluginsCopy[*match].id;
        createDeviceForPlugin(*match, nullptr);
        {
            std::lock_guard lock(pluginMutex_);
            pluginScanMessage_ = std::format("Auto-instantiated {}", pluginsCopy[*match].name);
        }
    } else {
        if (!defaults_.pluginName.empty()) {
            std::lock_guard lock(pluginMutex_);
            pluginScanMessage_ = std::format("Default plugin '{}' not found", defaults_.pluginName);
        }
    }
    pendingDefaultDevice_ = false;
}

void MainWindow::stopAllDevices() {
    std::vector<std::shared_ptr<DeviceState>> states;
    {
        std::lock_guard lock(devicesMutex_);
        for (auto& entry : devices_) {
            states.push_back(entry.state);
        }
        devices_.clear();
    }

    for (auto& state : states) {
        std::lock_guard guard(state->mutex);
        auto device = state->device;
        auto* sequencer = controller_.sequencer();
        for (auto& [instanceId, pluginState] : state->pluginInstances) {
            if (sequencer) {
                sequencer->destroyPluginUI(instanceId);
            }
            if (pluginState.pluginWindow) {
                pluginState.pluginWindow->show(false);
                pluginState.pluginWindow.reset();
            }
        }
        if (device) {
            device->stop();
            controller_.removeDevice(device->instanceId());
            state->device.reset();
        }
    }
}

bool MainWindow::handlePluginResizeRequest(std::shared_ptr<DeviceState> state, int32_t instanceId, uint32_t width, uint32_t height) {
    std::lock_guard guard(state->mutex);
    auto* nodeState = findPluginInstance(*state, instanceId);
    if (!nodeState || !nodeState->pluginWindow) {
        return false;
    }

    auto* window = nodeState->pluginWindow.get();
    if (!window) {
        return false;
    }

    auto& bounds = nodeState->pluginWindowBounds;
    bounds.width = static_cast<int>(width);
    bounds.height = static_cast<int>(height);

    if (nodeState->pluginWindowResizeIgnore) {
        nodeState->pluginWindowResizeIgnore = false;
        return true;
    }

    auto* sequencer = controller_.sequencer();
    if (!sequencer) {
        return false;
    }

    nodeState->pluginWindowResizeIgnore = true;
    if (!sequencer->resizePluginUI(instanceId, width, height)) {
        uint32_t adjustedWidth = 0, adjustedHeight = 0;
        sequencer->getPluginUISize(instanceId, adjustedWidth, adjustedHeight);
        if (adjustedWidth > 0 && adjustedHeight > 0) {
            bounds.width = static_cast<int>(adjustedWidth);
            bounds.height = static_cast<int>(adjustedHeight);
        }
    }

    remidy::EventLoop::runTaskOnMainThread([cw=window, w=bounds.width, h=bounds.height]() {
        if (cw) {
            cw->resize(w, h);
        }
    });

    return true;
}

void MainWindow::onPluginWindowResized(std::shared_ptr<DeviceState> state, int32_t instanceId) {
    std::lock_guard guard(state->mutex);
    auto* nodeState = findPluginInstance(*state, instanceId);
    if (!nodeState || !nodeState->pluginWindow || nodeState->pluginWindowResizeIgnore) {
        return;
    }

    auto* window = nodeState->pluginWindow.get();
    if (!window) {
        return;
    }

    auto bounds = window->getBounds();
    nodeState->pluginWindowBounds = bounds;

    auto* sequencer = controller_.sequencer();
    if (!sequencer) {
        return;
    }

    sequencer->resizePluginUI(instanceId,
        static_cast<uint32_t>(bounds.width),
        static_cast<uint32_t>(bounds.height));
}

void MainWindow::onPluginWindowClosed(std::shared_ptr<DeviceState> state, int32_t instanceId) {
    hidePluginUIInstance(state, instanceId);
}

void MainWindow::showPluginUIInstance(std::shared_ptr<DeviceState> state, int32_t instanceId) {
    auto weakState = std::weak_ptr(state);
    remidy::gui::ContainerWindow* container = nullptr;
    bool hasWindowNow = false;

    {
        std::lock_guard guard(state->mutex);
        auto* nodeState = findPluginInstance(*state, instanceId);
        if (!nodeState) {
            return;
        }
        if (!nodeState->pluginWindow) {
            std::string title = nodeState->pluginName.empty()
                ? std::format("Plugin {}", instanceId)
                : std::format("{} ({})", nodeState->pluginName, nodeState->pluginFormat);
            auto window = remidy::gui::ContainerWindow::create(title.c_str(), 800, 600, [this, weakState, instanceId]() {
                if (auto locked = weakState.lock()) {
                    onPluginWindowClosed(locked, instanceId);
                }
            });
            if (!window) {
                return;
            }
            container = window.get();
            nodeState->pluginWindow = std::move(window);
            nodeState->pluginWindowBounds = {100, 100, 800, 600};
            hasWindowNow = true;
        } else {
            container = nodeState->pluginWindow.get();
            if (nodeState->pluginWindowBounds.width == 0 || nodeState->pluginWindowBounds.height == 0) {
                nodeState->pluginWindowBounds = {100, 100, 800, 600};
            }
            hasWindowNow = true;
        }
    }

    if (!container || !hasWindowNow) {
        return;
    }

    container->show(true);
    void* parentHandle = container->getHandle();

    AudioPluginSequencer* sequencer = nullptr;
    bool pluginUIExists = false;
    {
        std::lock_guard guard(state->mutex);
        sequencer = controller_.sequencer();
        auto* nodeState = findPluginInstance(*state, instanceId);
        if (nodeState) {
            pluginUIExists = nodeState->pluginWindowEmbedded;
        }
    }
    if (!sequencer) {
        return;
    }

    if (!pluginUIExists) {
        // First time: create plugin UI with resize handler
        if (!sequencer->createPluginUI(instanceId, false, parentHandle,
            [this, weakState, instanceId](uint32_t w, uint32_t h) {
                if (auto locked = weakState.lock()) {
                    return handlePluginResizeRequest(locked, instanceId, w, h);
                }
                return false;
            })) {
            container->show(false);
            std::lock_guard guard(state->mutex);
            auto* nodeState = findPluginInstance(*state, instanceId);
            if (nodeState) {
                nodeState->pluginWindow.reset();
            }
            return;
        }

        std::lock_guard guard(state->mutex);
        auto* nodeState = findPluginInstance(*state, instanceId);
        if (nodeState) {
            nodeState->pluginWindowEmbedded = true;
        }
    }

    // Show the plugin UI (whether just created or already exists)
    if (!sequencer->showPluginUI(instanceId, false, parentHandle)) {
        std::cout << "Failed to show plugin UI for instance " << instanceId << std::endl;
    }
}

void MainWindow::hidePluginUIInstance(std::shared_ptr<DeviceState> state, int32_t instanceId) {
    std::unique_lock guard(state->mutex);
    auto* sequencer = controller_.sequencer();
    auto* nodeState = findPluginInstance(*state, instanceId);

    if (sequencer) {
        sequencer->hidePluginUI(instanceId);
    }

    if (nodeState && nodeState->pluginWindow) {
        nodeState->pluginWindow->show(false);
        // Keep the window alive for reuse, but ensure it's hidden before releasing the lock
    }
}
MainWindow::PluginInstanceState* MainWindow::findPluginInstance(DeviceState& state, int32_t instanceId) {
    auto it = state.pluginInstances.find(instanceId);
    if (it != state.pluginInstances.end()) {
        return &it->second;
    }

    auto [insertIt, _] = state.pluginInstances.emplace(instanceId, PluginInstanceState{});
    auto& pluginState = insertIt->second;
    pluginState.instanceId = instanceId;
    pluginState.statusMessage = "Running";
    pluginState.instantiating = false;
    pluginState.hasError = false;

    if (auto* sequencer = controller_.sequencer()) {
        pluginState.pluginName = sequencer->getPluginName(instanceId);
        pluginState.pluginFormat = sequencer->getPluginFormat(instanceId);
        pluginState.trackIndex = sequencer->findTrackIndexForInstance(instanceId);
    } else {
        pluginState.pluginName = std::format("Instance {}", instanceId);
        pluginState.pluginFormat.clear();
        pluginState.trackIndex = -1;
    }
    pluginState.pluginId.clear();
    return &pluginState;
}

std::shared_ptr<MainWindow::DeviceState> MainWindow::findDeviceById(int deviceEntryId) {
    std::lock_guard lock(devicesMutex_);
    for (auto& entry : devices_) {
        if (entry.id == deviceEntryId) {
            return entry.state;
        }
    }
    return nullptr;
}

} // namespace uapmd::service::gui
