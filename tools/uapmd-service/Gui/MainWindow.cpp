#include "MainWindow.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <imgui.h>
#include <optional>
#include <ranges>

#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"
#include <remidy/priv/event-loop.hpp>

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
                    .pluginId = entry->pluginId(),
                    .displayName = entry->displayName(),
                    .vendor = entry->vendorName()
                };
                collected.push_back(std::move(item));
            }
            std::sort(collected.begin(), collected.end(),
                      [](const PluginEntry& a, const PluginEntry& b) { return a.displayName < b.displayName; });
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
            selectedPluginId_ = plugins_[static_cast<size_t>(selectedPlugin_)].pluginId;
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
        auto name = toLower(plugins[i].displayName);
        auto vendor = toLower(plugins[i].vendor);
        auto pluginId = toLower(plugins[i].pluginId);
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
                                case 1: delta = a.displayName.compare(b.displayName); break;
                                case 2: delta = a.vendor.compare(b.vendor); break;
                                case 3: delta = a.pluginId.compare(b.pluginId); break;
                                default: break;
                            }
                            if (delta != 0) {
                                return (s->SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0) : (delta > 0);
                            }
                        }
                        // Tiebreaker for deterministic order
                        if (int t = a.displayName.compare(b.displayName); t != 0) return t < 0;
                        if (int t = a.vendor.compare(b.vendor); t != 0) return t < 0;
                        if (int t = a.pluginId.compare(b.pluginId); t != 0) return t < 0;
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
            bool selected = (selectedPluginFormat_ == plugin.format && selectedPluginId_ == plugin.pluginId);
            std::string selectableId = std::format("##{}::{}::{}", plugin.format, plugin.pluginId, index);
            if (ImGui::Selectable(selectableId.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedPlugin_ = index;
                selectedPluginFormat_ = plugin.format;
                selectedPluginId_ = plugin.pluginId;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(plugin.format.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(plugin.displayName.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(plugin.vendor.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(plugin.pluginId.c_str());
        }

        ImGui::EndTable();
    }

    ImGui::InputText("API (optional)", apiInput_.data(), apiInput_.size());
    ImGui::InputText("Device name", deviceNameInput_.data(), deviceNameInput_.size());

    // Build track destination options
    {
        std::vector<DeviceEntry> devicesCopy;
        {
            std::lock_guard lock(devicesMutex_);
            devicesCopy = devices_;
        }
        trackOptions_.clear();
        for (const auto& entry : devicesCopy) {
            auto state = entry.state;
            std::vector<AudioPluginSequencer::TrackInfo> tracks;
            std::string label;
            {
                std::lock_guard guard(state->mutex);
                label = state->label;
                auto* device = state->device.get();
                auto* sequencer = device ? device->sequencer() : nullptr;
                if (sequencer) {
                    tracks = sequencer->getTrackInfos();
                }
            }
            for (const auto& track : tracks) {
                TrackDestinationOption option{
                    .deviceEntryId = entry.id,
                    .trackIndex = track.trackIndex,
                    .label = std::format("{} — Track {}", label, track.trackIndex + 1)
                };
                trackOptions_.push_back(std::move(option));
            }
        }
        if (selectedTrackOption_ < 0 || selectedTrackOption_ > static_cast<int>(trackOptions_.size())) {
            selectedTrackOption_ = 0;
        }

        std::vector<std::string> labels;
        labels.reserve(trackOptions_.size() + 1);
        labels.emplace_back("New track (new UMP device)");
        for (const auto& option : trackOptions_) {
            labels.push_back(option.label);
        }
        std::vector<const char*> labelPtrs;
        labelPtrs.reserve(labels.size());
        for (auto& label : labels) {
            labelPtrs.push_back(label.c_str());
        }
        ImGui::Combo("Track destination", &selectedTrackOption_, labelPtrs.data(), static_cast<int>(labelPtrs.size()));
    }

    bool canCreate = selectedPlugin_ >= 0 && selectedPlugin_ < static_cast<int>(pluginsCopy.size());
    if (!pluginScanCompleted_) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Create UMP Device") && canCreate) {
        if (selectedTrackOption_ > 0 && static_cast<size_t>(selectedTrackOption_ - 1) < trackOptions_.size()) {
            addPluginToExistingTrack(static_cast<size_t>(selectedPlugin_), trackOptions_[static_cast<size_t>(selectedTrackOption_ - 1)]);
        } else {
            createDeviceForPlugin(static_cast<size_t>(selectedPlugin_));
        }
    }
    if (!pluginScanCompleted_) {
        ImGui::EndDisabled();
    }
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

    for (size_t deviceIdx = 0; deviceIdx < devicesCopy.size(); ++deviceIdx) {
        auto entry = devicesCopy[deviceIdx];
        auto state = entry.state;

        std::vector<AudioPluginSequencer::TrackInfo> tracks;
        std::string label;
        std::string apiName;
        std::string status;
        bool running = false;
        bool instantiating = false;
        bool hasError = false;

        {
            std::lock_guard guard(state->mutex);
            label = state->label;
            apiName = state->apiName;
            status = state->statusMessage;
            running = state->running;
            instantiating = state->instantiating;
            hasError = state->hasError;
            auto* device = state->device.get();
            auto* sequencer = device ? device->sequencer() : nullptr;
            if (sequencer) {
                tracks = sequencer->getTrackInfos();
            }
        }

        if (tracks.empty()) {
            ImGui::PushID(std::format("{}-pending", entry.id).c_str());
            ImGui::Separator();
            ImGui::Text("%s — Track pending", label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", apiName.c_str());

            ImGui::SameLine();
            if (running && !instantiating) {
                if (ImGui::Button("Stop")) {
                    std::lock_guard guard(state->mutex);
                    if (state->device) {
                        state->device->stop();
                    }
                    state->running = false;
                    state->statusMessage = "Stopped";
                }
            } else {
                if (ImGui::Button("Start")) {
                    int statusCode = 0;
                    std::lock_guard guard(state->mutex);
                    if (state->device) {
                        statusCode = state->device->start();
                    }
                    if (statusCode == 0) {
                        state->running = true;
                        state->statusMessage = "Running";
                        state->hasError = false;
                    } else {
                        state->statusMessage = std::format("Start failed (status {})", statusCode);
                        state->hasError = true;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                removeIndex = deviceIdx;
            }

            if (instantiating) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", status.c_str());
            } else if (hasError) {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", status.c_str());
            } else {
                ImGui::TextUnformatted(status.c_str());
            }
            ImGui::PopID();
            continue;
        }

        for (const auto& track : tracks) {
            ImGui::PushID(std::format("{}-track{}", entry.id, track.trackIndex).c_str());
            ImGui::Separator();
            ImGui::Text("%s — Track %d", label.c_str(), track.trackIndex + 1);
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", apiName.c_str());

            ImGui::SameLine();
            if (running && !instantiating) {
                if (ImGui::Button("Stop")) {
                    std::lock_guard guard(state->mutex);
                    if (state->device) {
                        state->device->stop();
                    }
                    state->running = false;
                    state->statusMessage = "Stopped";
                }
            } else {
                if (ImGui::Button("Start")) {
                    int statusCode = 0;
                    std::lock_guard guard(state->mutex);
                    if (state->device) {
                        statusCode = state->device->start();
                    }
                    if (statusCode == 0) {
                        state->running = true;
                        state->statusMessage = "Running";
                        state->hasError = false;
                    } else {
                        state->statusMessage = std::format("Start failed (status {})", statusCode);
                        state->hasError = true;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                removeIndex = deviceIdx;
            }

            if (instantiating) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", status.c_str());
            } else if (hasError) {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", status.c_str());
            } else {
                ImGui::TextUnformatted(status.c_str());
            }

            if (track.nodes.empty()) {
                ImGui::TextDisabled("No plugin instances on this track.");
            } else if (ImGui::BeginTable("TrackPlugins", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("Plugin");
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Instance", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableHeadersRow();

                for (const auto& node : track.nodes) {
                    std::string nodeStatus = "Running";
                    bool nodeInstantiating = false;
                    bool nodeHasError = false;
                    bool uiSupported = false;
                    bool uiVisible = false;
                    {
                        std::lock_guard guard(state->mutex);
                        auto* pluginState = findPluginInstance(*state, node.instanceId);
                        if (pluginState) {
                            pluginState->pluginName = node.displayName;
                            pluginState->pluginFormat = node.format;
                            pluginState->pluginId = node.pluginId;
                            pluginState->trackIndex = track.trackIndex;
                            nodeStatus = pluginState->statusMessage;
                            nodeInstantiating = pluginState->instantiating;
                            nodeHasError = pluginState->hasError;
                        }
                        auto* device = state->device.get();
                        auto* sequencer = device ? device->sequencer() : nullptr;
                        if (sequencer && node.instanceId >= 0) {
                            uiSupported = sequencer->hasPluginUI(node.instanceId);
                            if (uiSupported) {
                                uiVisible = sequencer->isPluginUIVisible(node.instanceId);
                            }
                        }
                    }

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(node.displayName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(node.format.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", node.instanceId);

                    ImGui::TableSetColumnIndex(3);
                    if (nodeInstantiating) {
                        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", nodeStatus.c_str());
                    } else if (nodeHasError) {
                        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", nodeStatus.c_str());
                    } else {
                        ImGui::TextUnformatted(nodeStatus.c_str());
                    }

                    ImGui::TableSetColumnIndex(4);
                    if (running && !instantiating && uiSupported) {
                        const char* uiText = uiVisible ? "Hide UI" : "Show UI";
                        std::string btnId = std::format("{}##{}::{}", uiText, entry.id, node.instanceId);
                        if (ImGui::Button(btnId.c_str())) {
                            if (uiVisible) {
                                hidePluginUIInstance(state, node.instanceId);
                            } else {
                                showPluginUIInstance(state, node.instanceId);
                            }
                        }
                        ImGui::SameLine();
                    }

                    std::string removeBtnId = std::format("Remove##{}::{}", entry.id, node.instanceId);
                    if (ImGui::Button(removeBtnId.c_str())) {
                        hidePluginUIInstance(state, node.instanceId);
                        bool removed = false;
                        {
                            std::lock_guard guard(state->mutex);
                            if (state->device) {
                                removed = state->device->removePluginInstance(node.instanceId);
                            }
                            if (removed) {
                                state->pluginInstances.erase(node.instanceId);
                                state->statusMessage = std::format("Removed {}", node.displayName);
                                state->hasError = false;
                            } else {
                                state->statusMessage = std::format("Failed to remove {}", node.displayName);
                                state->hasError = true;
                            }
                        }
                    }

                    if (running && !instantiating) {
                        // Placeholder for future per-plugin actions
                    }
                }

                ImGui::EndTable();
            }

            ImGui::PopID();
        }
    }

    if (removeIndex != static_cast<size_t>(-1)) {
        removeDevice(removeIndex);
    }
}

void MainWindow::createDeviceForPlugin(size_t pluginIndex) {
    PluginEntry plugin;
    {
        std::lock_guard lock(pluginMutex_);
        if (pluginIndex >= plugins_.size()) {
            return;
        }
        plugin = plugins_[pluginIndex];
        selectedPluginFormat_ = plugin.format;
        selectedPluginId_ = plugin.pluginId;
    }

    auto state = std::make_shared<DeviceState>();
    state->apiName = bufferToString(apiInput_);
    if (state->apiName.empty()) {
        state->apiName = defaults_.apiName;
    }
    state->label = bufferToString(deviceNameInput_);
    if (state->label.empty()) {
        state->label = std::format("{} [{}]", plugin.displayName, plugin.format);
    }
    state->statusMessage = "Instantiating plugin...";
    state->instantiating = true;

    auto device = controller_.createDevice(state->apiName, state->label, defaultManufacturer_, defaultVersion_);
    auto* devicePtr = device.get();
    state->device = std::move(device);

    std::weak_ptr<DeviceState> weakState = state;
    std::string format = plugin.format;
    std::string pluginId = plugin.pluginId;
    std::string pluginName = plugin.displayName;

    devicePtr->addPluginTrackById(format, pluginId,
        [this, weakState, pluginName, format, pluginId](int32_t instanceId, std::string error) {
            if (auto locked = weakState.lock()) {
                std::lock_guard guard(locked->mutex);
                if (!error.empty()) {
                    locked->statusMessage = "Plugin instantiation failed: " + error;
                    locked->hasError = true;
                    locked->instantiating = false;
                } else {
                    locked->statusMessage = "Running";
                    locked->instantiating = false;
                    locked->hasError = false;
                    auto& node = locked->pluginInstances[instanceId];
                    node.instanceId = instanceId;
                    node.pluginName = pluginName;
                    node.pluginFormat = format;
                    node.pluginId = pluginId;
                    node.statusMessage = std::format("Plugin ready (instance {})", instanceId);
                    node.instantiating = false;
                    node.hasError = false;
                    if (locked->device && locked->device->sequencer()) {
                        node.trackIndex = locked->device->sequencer()->findTrackIndexForInstance(instanceId);
                    } else {
                        node.trackIndex = 0;
                    }
                }
            }
        });

    int startStatus = devicePtr->start();
    {
        std::lock_guard guard(state->mutex);
        if (startStatus == 0) {
            state->running = true;
            state->statusMessage = "Running";
        } else {
            state->running = false;
            state->hasError = true;
            state->instantiating = false;
            state->statusMessage = std::format("Failed to start device (status {})", startStatus);
        }
    }

    {
        std::lock_guard lock(devicesMutex_);
        devices_.push_back(DeviceEntry{nextDeviceId_++, state});
    }
}

void MainWindow::addPluginToExistingTrack(size_t pluginIndex, const TrackDestinationOption& destination) {
    PluginEntry plugin;
    {
        std::lock_guard lock(pluginMutex_);
        if (pluginIndex >= plugins_.size()) {
            return;
        }
        plugin = plugins_[pluginIndex];
    }

    auto target = findDeviceById(destination.deviceEntryId);
    if (!target) {
        return;
    }

    std::string format = plugin.format;
    std::string pluginId = plugin.pluginId;
    std::string pluginName = plugin.displayName;
    std::weak_ptr<DeviceState> weakState = target;

    UapmdMidiDevice* devicePtr = nullptr;
    {
        std::lock_guard guard(target->mutex);
        devicePtr = target->device.get();
        if (!devicePtr) {
            target->statusMessage = "Target device unavailable";
            target->hasError = true;
            target->instantiating = false;
            return;
        }
        target->instantiating = true;
        target->statusMessage = std::format("Adding {} to track {}", plugin.displayName, destination.trackIndex + 1);
    }

    devicePtr->addPluginToTrackById(destination.trackIndex, format, pluginId,
        [this, weakState, pluginName, format, pluginId, destination](int32_t instanceId, std::string error) {
            if (auto locked = weakState.lock()) {
                std::lock_guard guard(locked->mutex);
                if (!error.empty()) {
                    locked->statusMessage = "Plugin append failed: " + error;
                    locked->hasError = true;
                    locked->instantiating = false;
                    return;
                }
                locked->instantiating = false;
                locked->hasError = false;
                locked->statusMessage = "Running";
                auto& node = locked->pluginInstances[instanceId];
                node.instanceId = instanceId;
                node.pluginName = pluginName;
                node.pluginFormat = format;
                node.pluginId = pluginId;
                node.statusMessage = std::format("Plugin ready (instance {})", instanceId);
                node.instantiating = false;
                node.hasError = false;
                if (locked->device && locked->device->sequencer()) {
                    node.trackIndex = locked->device->sequencer()->findTrackIndexForInstance(instanceId);
                } else {
                    node.trackIndex = destination.trackIndex;
                }
            }
        });
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
        auto* device = state->device.get();
        auto* sequencer = device ? device->sequencer() : nullptr;
        for (auto& [instanceId, pluginState] : state->pluginInstances) {
            if (sequencer) {
                sequencer->hidePluginUI(instanceId);
                sequencer->setPluginUIResizeHandler(instanceId, nullptr);
            }
            if (pluginState.pluginWindow) {
                pluginState.pluginWindow->show(false);
                pluginState.pluginWindow.reset();
            }
        }
        if (state->device) {
            state->device->stop();
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
        auto entryName = toLower(entry.displayName);
        auto entryId = toLower(entry.pluginId);
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
        selectedPluginId_ = pluginsCopy[*match].pluginId;
        createDeviceForPlugin(*match);
        {
            std::lock_guard lock(pluginMutex_);
            pluginScanMessage_ = std::format("Auto-instantiated {}", pluginsCopy[*match].displayName);
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
        auto* device = state->device.get();
        auto* sequencer = device ? device->sequencer() : nullptr;
        for (auto& [instanceId, pluginState] : state->pluginInstances) {
            if (sequencer) {
                sequencer->hidePluginUI(instanceId);
                sequencer->setPluginUIResizeHandler(instanceId, nullptr);
            }
            if (pluginState.pluginWindow) {
                pluginState.pluginWindow->show(false);
                pluginState.pluginWindow.reset();
            }
        }
        if (device) {
            device->stop();
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

    auto* device = state->device.get();
    if (!device || !device->sequencer()) {
        return false;
    }

    nodeState->pluginWindowResizeIgnore = true;
    auto sequencer = device->sequencer();
    bool canResize = sequencer->canPluginUIResize(instanceId);
    if (!sequencer->resizePluginUI(instanceId, width, height)) {
        uint32_t adjustedWidth = 0, adjustedHeight = 0;
        sequencer->getPluginUISize(instanceId, adjustedWidth, adjustedHeight);
        if (adjustedWidth > 0 && adjustedHeight > 0) {
            bounds.width = static_cast<int>(adjustedWidth);
            bounds.height = static_cast<int>(adjustedHeight);
        }
    }

    remidy::EventLoop::runTaskOnMainThread([cw=window, bounds, canResize]() {
        if (cw) {
            cw->setResizable(canResize);
            cw->setBounds(bounds);
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

    auto* device = state->device.get();
    if (!device || !device->sequencer()) {
        return;
    }

    device->sequencer()->resizePluginUI(instanceId,
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
            auto window = remidy::gui::ContainerWindow::create(title.c_str(), 800, 600);
            if (!window) {
                return;
            }
            container = window.get();
            window->setCloseCallback([this, weakState, instanceId]() {
                if (auto locked = weakState.lock()) {
                    onPluginWindowClosed(locked, instanceId);
                }
            });
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
    {
        std::lock_guard guard(state->mutex);
        auto* device = state->device.get();
        sequencer = device ? device->sequencer() : nullptr;
    }
    if (!sequencer) {
        return;
    }

    sequencer->setPluginUIResizeHandler(instanceId,
        [this, weakState, instanceId](uint32_t w, uint32_t h) {
            if (auto locked = weakState.lock()) {
                return handlePluginResizeRequest(locked, instanceId, w, h);
            }
            return false;
        });

    if (sequencer->showPluginUI(instanceId, false, parentHandle)) {
        uint32_t pw = 0, ph = 0;
        bool hasSize = sequencer->getPluginUISize(instanceId, pw, ph) && pw > 0 && ph > 0;
        bool canResize = sequencer->canPluginUIResize(instanceId);
        remidy::gui::Bounds bounds{};
        bool applyBounds = false;
        {
            std::lock_guard guard(state->mutex);
            auto* nodeState = findPluginInstance(*state, instanceId);
            if (nodeState) {
                nodeState->pluginWindowEmbedded = true;
                if (hasSize) {
                    nodeState->pluginWindowBounds.width = static_cast<int>(pw);
                    nodeState->pluginWindowBounds.height = static_cast<int>(ph);
                    nodeState->pluginWindowResizeIgnore = true;
                    bounds = nodeState->pluginWindowBounds;
                    applyBounds = true;
                }
            }
        }
        if (applyBounds) {
            remidy::EventLoop::runTaskOnMainThread([cw=container, bounds, canResize]() {
                if (cw) {
                    cw->setResizable(canResize);
                    cw->setBounds(bounds);
                }
            });
        }
    } else {
        container->show(false);
        sequencer->setPluginUIResizeHandler(instanceId, nullptr);
        bool hadEmbeddedWindow = false;
        {
            std::lock_guard guard(state->mutex);
            auto* nodeState = findPluginInstance(*state, instanceId);
            if (nodeState) {
                hadEmbeddedWindow = nodeState->pluginWindowEmbedded;
                nodeState->pluginWindowEmbedded = false;
                nodeState->pluginWindowResizeIgnore = false;
                nodeState->pluginWindow.reset();
            }
        }
        if (sequencer->showPluginUI(instanceId, true, nullptr)) {
            std::lock_guard guard(state->mutex);
            auto* nodeState = findPluginInstance(*state, instanceId);
            if (nodeState) {
                nodeState->pluginWindowEmbedded = false;
                nodeState->pluginWindowResizeIgnore = false;
            }
        }
    }
}

void MainWindow::hidePluginUIInstance(std::shared_ptr<DeviceState> state, int32_t instanceId) {
    std::unique_lock guard(state->mutex);
    auto* device = state->device.get();
    auto* sequencer = device ? device->sequencer() : nullptr;
    auto* nodeState = findPluginInstance(*state, instanceId);

    if (sequencer) {
        sequencer->hidePluginUI(instanceId);
        sequencer->setPluginUIResizeHandler(instanceId, nullptr);
    }

    if (nodeState && nodeState->pluginWindow) {
        nodeState->pluginWindow->show(false);
        // Keep the window alive for reuse, but ensure it’s hidden before releasing the lock
    }
    if (nodeState) {
        nodeState->pluginWindowResizeIgnore = false;
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

    auto* device = state.device.get();
    if (device && device->sequencer()) {
        pluginState.pluginName = device->sequencer()->getPluginName(instanceId);
        pluginState.pluginFormat = device->sequencer()->getPluginFormat(instanceId);
        pluginState.trackIndex = device->sequencer()->findTrackIndexForInstance(instanceId);
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
