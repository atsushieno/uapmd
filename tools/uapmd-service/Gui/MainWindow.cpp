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

    bool canCreate = selectedPlugin_ >= 0 && selectedPlugin_ < static_cast<int>(pluginsCopy.size());
    if (!pluginScanCompleted_) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Create UMP Device") && canCreate) {
        createDeviceForPlugin(static_cast<size_t>(selectedPlugin_));
    }
    if (!pluginScanCompleted_) {
        ImGui::EndDisabled();
    }
}

void MainWindow::renderDeviceManager() {
    ImGui::TextUnformatted("Active UMP devices");
    std::vector<DeviceEntry> devicesCopy;
    {
        std::lock_guard lock(devicesMutex_);
        devicesCopy = devices_;
    }

    if (devicesCopy.empty()) {
        ImGui::TextDisabled("No virtual MIDI devices created yet.");
        return;
    }

    if (ImGui::BeginTable("DeviceTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Plugin");
        ImGui::TableSetupColumn("API", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableHeadersRow();

        size_t removeIndex = static_cast<size_t>(-1);

        for (size_t i = 0; i < devicesCopy.size(); ++i) {
            auto state = devicesCopy[i].state;
            std::string label;
            std::string pluginInfo;
            std::string status;
            std::string apiName;
            bool running = false;
            bool instantiating = false;
            bool hasError = false;

            {
                std::lock_guard guard(state->mutex);
                label = state->label;
                pluginInfo = std::format("{} ({})", state->pluginName, state->pluginFormat);
                status = state->statusMessage;
                apiName = state->apiName;
                running = state->running;
                instantiating = state->instantiating;
                hasError = state->hasError;
            }

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(pluginInfo.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(apiName.c_str());

            ImGui::TableSetColumnIndex(3);
            if (instantiating) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", status.c_str());
            } else if (hasError) {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", status.c_str());
            } else {
                ImGui::TextUnformatted(status.c_str());
            }

            ImGui::TableSetColumnIndex(4);
            if (running && !instantiating) {
                // Show/Hide UI button
                auto* device = state->device.get();
                if (device && device->sequencer()) {
                    auto* sequencer = device->sequencer();
                    int32_t instanceId = state->instanceId;
                    if (instanceId >= 0 && sequencer->hasPluginUI(instanceId)) {
                        bool isVisible = sequencer->isPluginUIVisible(instanceId);
                        const char* uiText = isVisible ? "Hide UI" : "Show UI";
                        std::string btnId = std::format("{}##{}", uiText, devicesCopy[i].id);

                        if (ImGui::Button(btnId.c_str())) {
                            if (isVisible) {
                                // Hide UI
                                sequencer->hidePluginUI(instanceId);
                                if (state->pluginWindow) {
                                    state->pluginWindow->show(false);
                                }
                                sequencer->setPluginUIResizeHandler(instanceId, nullptr);
                                state->pluginWindowResizeIgnore = false;
                            } else {
                                // Show UI
                                bool shown = false;
                                remidy::gui::ContainerWindow* container = nullptr;

                                if (!state->pluginWindow) {
                                    std::string title = std::format("{} ({})",
                                        state->pluginName, state->pluginFormat);
                                    auto w = remidy::gui::ContainerWindow::create(
                                        title.c_str(), 800, 600);
                                    container = w.get();
                                    w->setCloseCallback([this, weakState = std::weak_ptr(state)]() {
                                        if (auto s = weakState.lock()) {
                                            onPluginWindowClosed(s);
                                        }
                                    });
                                    state->pluginWindow = std::move(w);
                                    state->pluginWindowBounds = {100, 100, 800, 600};
                                } else {
                                    container = state->pluginWindow.get();
                                }

                                if (container) {
                                    container->show(true);
                                    void* parentHandle = container->getHandle();

                                    sequencer->setPluginUIResizeHandler(instanceId,
                                        [this, weakState = std::weak_ptr(state)](uint32_t w, uint32_t h) {
                                            if (auto s = weakState.lock()) {
                                                return handlePluginResizeRequest(s, w, h);
                                            }
                                            return false;
                                        });

                                    if (sequencer->showPluginUI(instanceId, false, parentHandle)) {
                                        state->pluginWindowEmbedded = true;
                                        uint32_t pw = 0, ph = 0;
                                        if (fetchPluginUISize(state, pw, ph) && pw > 0 && ph > 0) {
                                            state->pluginWindowBounds.width = static_cast<int>(pw);
                                            state->pluginWindowBounds.height = static_cast<int>(ph);
                                            state->pluginWindowResizeIgnore = true;

                                            auto bounds = state->pluginWindowBounds;
                                            bool canResize = sequencer->canPluginUIResize(instanceId);
                                            remidy::EventLoop::runTaskOnMainThread(
                                                [cw=container, bounds, canResize]() {
                                                    if (cw) {
                                                        cw->setResizable(canResize);
                                                        cw->setBounds(bounds);
                                                    }
                                                });
                                        }
                                        shown = true;
                                    } else {
                                        // Embedded failed, cleanup
                                        container->show(false);
                                        state->pluginWindow.reset();
                                        sequencer->setPluginUIResizeHandler(instanceId, nullptr);
                                    }
                                }

                                if (!shown) {
                                    // Fallback to floating window
                                    if (sequencer->showPluginUI(instanceId, true, nullptr)) {
                                        state->pluginWindowEmbedded = false;
                                        sequencer->setPluginUIResizeHandler(instanceId, nullptr);
                                        state->pluginWindowResizeIgnore = false;
                                    }
                                }
                            }
                        }
                        ImGui::SameLine();
                    }
                }

                // Stop button
                if (ImGui::Button(std::format("Stop##{}", devicesCopy[i].id).c_str())) {
                    if (auto locked = state) {
                        std::lock_guard guard(state->mutex);
                        if (state->device) {
                            state->device->stop();
                        }
                        state->running = false;
                        state->statusMessage = "Stopped";
                    }
                }
                ImGui::SameLine();
            } else if (!running) {
                if (ImGui::Button(std::format("Start##{}", devicesCopy[i].id).c_str())) {
                    int statusCode = 0;
                    if (auto locked = state) {
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
            }

            if (ImGui::Button(std::format("Remove##{}", devicesCopy[i].id).c_str())) {
                removeIndex = i;
            }
        }

        ImGui::EndTable();

        if (removeIndex != static_cast<size_t>(-1)) {
            removeDevice(removeIndex);
        }
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
    state->pluginName = plugin.displayName;
    state->pluginFormat = plugin.format;
    state->pluginId = plugin.pluginId;
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

    devicePtr->addPluginTrackById(format, pluginId,
        [weakState](int32_t instanceId, std::string error) {
            if (auto locked = weakState.lock()) {
                std::lock_guard guard(locked->mutex);
                if (!error.empty()) {
                    locked->statusMessage = "Plugin instantiation failed: " + error;
                    locked->hasError = true;
                    locked->instantiating = false;
                } else {
                    locked->statusMessage = std::format("Plugin ready (instance {})", instanceId);
                    locked->instantiating = false;
                    locked->hasError = false;
                    locked->instanceId = instanceId;
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
        // Clean up plugin window
        if (state->pluginWindow) {
            if (auto* device = state->device.get()) {
                auto sequencer = device->sequencer();
                if (sequencer) {
                    sequencer->hidePluginUI(state->instanceId);
                    sequencer->setPluginUIResizeHandler(state->instanceId, nullptr);
                }
            }
            state->pluginWindow.reset();
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
        if (state->device) {
            state->device->stop();
            state->device.reset();
        }
    }
}

bool MainWindow::handlePluginResizeRequest(std::shared_ptr<DeviceState> state, uint32_t width, uint32_t height) {
    std::lock_guard guard(state->mutex);
    if (!state->pluginWindow) {
        return false;
    }

    auto* window = state->pluginWindow.get();
    if (!window) {
        return false;
    }

    auto& bounds = state->pluginWindowBounds;
    remidy::gui::Bounds currentBounds = bounds;

    // Keep existing x/y from stored bounds
    bounds.width = static_cast<int>(width);
    bounds.height = static_cast<int>(height);

    // Don't process if this resize came from user window action
    if (state->pluginWindowResizeIgnore) {
        state->pluginWindowResizeIgnore = false;
        return true;
    }

    // Try to apply the requested size
    auto* device = state->device.get();
    if (!device || !device->sequencer()) {
        return false;
    }

    int32_t instanceId = state->instanceId;
    state->pluginWindowResizeIgnore = true;

    auto sequencer = device->sequencer();
    bool canResize = sequencer->canPluginUIResize(instanceId);
    if (!sequencer->resizePluginUI(instanceId, width, height)) {
        // Plugin rejected size, get what it wants
        uint32_t adjustedWidth = 0, adjustedHeight = 0;
        sequencer->getPluginUISize(instanceId, adjustedWidth, adjustedHeight);
        if (adjustedWidth > 0 && adjustedHeight > 0) {
            bounds.width = static_cast<int>(adjustedWidth);
            bounds.height = static_cast<int>(adjustedHeight);
        }
    }

    // Apply the bounds on the main thread
    remidy::EventLoop::runTaskOnMainThread([cw=window, bounds, canResize]() {
        if (cw) {
            cw->setResizable(canResize);
            cw->setBounds(bounds);
        }
    });

    return true;
}

void MainWindow::onPluginWindowResized(std::shared_ptr<DeviceState> state) {
    std::lock_guard guard(state->mutex);
    if (!state->pluginWindow || state->pluginWindowResizeIgnore) {
        return;
    }

    auto* window = state->pluginWindow.get();
    if (!window) {
        return;
    }

    auto bounds = window->getBounds();
    state->pluginWindowBounds = bounds;

    auto* device = state->device.get();
    if (!device || !device->sequencer()) {
        return;
    }

    int32_t instanceId = state->instanceId;
    device->sequencer()->resizePluginUI(instanceId,
        static_cast<uint32_t>(bounds.width),
        static_cast<uint32_t>(bounds.height));
}

void MainWindow::onPluginWindowClosed(std::shared_ptr<DeviceState> state) {
    std::lock_guard guard(state->mutex);
    auto* device = state->device.get();
    if (device && device->sequencer()) {
        auto sequencer = device->sequencer();
        int32_t instanceId = state->instanceId;
        sequencer->hidePluginUI(instanceId);
        sequencer->setPluginUIResizeHandler(instanceId, nullptr);
    }

    if (state->pluginWindow) {
        state->pluginWindow->show(false);
    }
    state->pluginWindowResizeIgnore = false;
}

bool MainWindow::fetchPluginUISize(std::shared_ptr<DeviceState> state, uint32_t& width, uint32_t& height) {
    std::lock_guard guard(state->mutex);
    auto* device = state->device.get();
    if (!device || !device->sequencer()) {
        return false;
    }

    return device->sequencer()->getPluginUISize(state->instanceId, width, height);
}

} // namespace uapmd::service::gui
