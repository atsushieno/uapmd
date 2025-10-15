#include "MainWindow.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <imgui.h>
#include <optional>
#include <ranges>

#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"

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
    if (ImGui::BeginTable("PluginTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(-FLT_MIN, tableHeight))) {
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableHeadersRow();

        for (int index : visible) {
            const auto& plugin = pluginsCopy[index];
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
            if (running) {
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
            } else {
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

} // namespace uapmd::service::gui
