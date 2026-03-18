#include "PluginSelector.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "../AppModel.hpp"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool kRemoteScannerSupported = false;
#else
constexpr bool kRemoteScannerSupported = true;
#endif

struct ParsedScanReportRow {
    std::string bundle;
    std::string timeSeconds;
    std::string plugins;
};

std::vector<ParsedScanReportRow> parseScanReportMarkdown(const std::string& markdown) {
    std::vector<ParsedScanReportRow> rows;
    std::istringstream stream(markdown);
    std::string line;
    bool headerSkipped = false;
    bool separatorSkipped = false;
    auto trim = [](std::string value) {
        auto start = value.find_first_not_of(" \t");
        auto end = value.find_last_not_of(" \t");
        if (start == std::string::npos || end == std::string::npos)
            return std::string{};
        return value.substr(start, end - start + 1);
    };
    auto unescapeCell = [](std::string value) {
        std::string result;
        result.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                result.push_back(value[++i]);
            } else {
                result.push_back(value[i]);
            }
        }
        std::string display;
        display.reserve(result.size());
        for (size_t i = 0; i < result.size();) {
            if (result.compare(i, 4, "<br>") == 0) {
                display.push_back('\n');
                i += 4;
            } else {
                display.push_back(result[i++]);
            }
        }
        return display;
    };
    while (std::getline(stream, line)) {
        if (line.rfind("| ---", 0) == 0) {
            separatorSkipped = true;
            continue;
        }
        if (line.empty() || line[0] != '|')
            continue;
        if (!headerSkipped) {
            headerSkipped = true;
            continue;
        }
        if (!separatorSkipped)
            continue;
        std::vector<std::string> cells;
        std::string cell;
        for (size_t i = 1; i < line.size(); ++i) {
            char c = line[i];
            if (c == '|') {
                cells.push_back(trim(cell));
                cell.clear();
                continue;
            }
            cell.push_back(c);
        }
        if (!cell.empty())
            cells.push_back(trim(cell));
        if (cells.size() < 3)
            continue;
        ParsedScanReportRow row{
            .bundle = unescapeCell(cells[0]),
            .timeSeconds = unescapeCell(cells[1]),
            .plugins = unescapeCell(cells[2])
        };
        rows.push_back(std::move(row));
    }
    return rows;
}
}

namespace uapmd::gui {

PluginSelector::PluginSelector() {
    uapmd::AppModel::instance().scanReportReady.push_back([this](const std::string& text) {
        scanReportText_ = text;
        refreshReportBuffer();
    });
}

void PluginSelector::render() {
    if (isScanning_) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Scan Plugins")) {
        if (onScanPlugins_) {
            double timeoutSeconds = remoteScanTimeoutSeconds_;
            if (timeoutSeconds < 0.0)
                timeoutSeconds = 0.0;
            onScanPlugins_(forceRescan_, useRemoteScanner_, timeoutSeconds);
        }
        std::cout << "Starting plugin scanning" << std::endl;
    }

    auto& appModel = uapmd::AppModel::instance();
    auto scanState = appModel.slowScanProgress();
    auto lastError = appModel.lastPluginScanError();

    if (isScanning_) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Scanning...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            appModel.cancelPluginScanning();
        }
    } else {
        ImGui::SameLine();
        ImGui::Checkbox("Force Rescan", &forceRescan_);
    }

    if constexpr (kRemoteScannerSupported) {
        ImGui::SameLine();
        if (isScanning_)
            ImGui::BeginDisabled();
        ImGui::Checkbox("Remote scanner process", &useRemoteScanner_);
        if (isScanning_)
            ImGui::EndDisabled();
        ImGui::SameLine();
        bool disableTimeout = isScanning_ || !useRemoteScanner_;
        if (disableTimeout)
            ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::InputDouble("Timeout (s)", &remoteScanTimeoutSeconds_, 0.0, 0.0, "%.1f"))
            remoteScanTimeoutSeconds_ = std::max(0.0, remoteScanTimeoutSeconds_);
        if (disableTimeout)
            ImGui::EndDisabled();
    } else {
        useRemoteScanner_ = false;
    }

    if (scanState.running) {
        if (scanState.totalBundles > 0) {
            float progress = static_cast<float>(scanState.processedBundles) / static_cast<float>(scanState.totalBundles);
            auto label = std::format("{} / {}", scanState.processedBundles, scanState.totalBundles);
            ImGui::ProgressBar(progress, ImVec2(-1, 0), label.c_str());
            if (scanState.totalBundles > scanState.processedBundles)
                ImGui::Text("Discovered %u bundle(s) so far", scanState.totalBundles);
        } else {
            ImGui::Text("Scanning... processed %u bundle(s)", scanState.processedBundles);
            if (scanState.totalBundles > scanState.processedBundles)
                ImGui::Text("Discovered %u bundle(s) so far", scanState.totalBundles);
        }
        if (!scanState.currentBundle.empty()) {
            ImGui::Text("Current bundle: %s", scanState.currentBundle.c_str());
        }
    } else if (!lastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Last scanning error: %s", lastError.c_str());
    }

    if (!scanState.running && scanState.processedBundles > 0) {
        if (scanState.totalBundles > 0) {
            ImGui::Text("Last scan processed %u / %u bundle(s).", scanState.processedBundles, scanState.totalBundles);
        } else {
            ImGui::Text("Last scan processed %u bundle(s).", scanState.processedBundles);
        }
        if (ImGui::Button("Show scan report")) {
            reportWindowOpen_ = true;
            if (scanReportText_.empty()) {
                scanReportText_ = uapmd::AppModel::instance().generateScanReport();
                refreshReportBuffer();
            } else if (scanReportBuffer_.size() <= 1) {
                refreshReportBuffer();
            }
        }
    }

    ImGui::Separator();

    auto blocklist = appModel.pluginBlocklist();
    const std::string blocklistLabel = std::format("{} Blocked plugin{}", blocklist.size(), blocklist.size() == 1 ? "" : "s");
    ImGui::SetNextItemOpen(false, ImGuiCond_FirstUseEver);
    if (ImGui::CollapsingHeader(blocklistLabel.c_str())) {
        if (blocklist.empty()) {
            ImGui::TextUnformatted("No blocklisted plugins.");
        } else {
            if (ImGui::Button("Clear blocklist")) {
                appModel.clearPluginBlocklist();
            }
            ImGui::BeginTable("blocked_plugins_table", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable);
            ImGui::TableSetupColumn("Format");
            ImGui::TableSetupColumn("Plugin ID");
            ImGui::TableSetupColumn("Reason");
            ImGui::TableSetupColumn("Timestamp");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (const auto& entry : blocklist) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry.format.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(entry.pluginId.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", entry.reason.c_str());
                ImGui::TableSetColumnIndex(3);
                std::time_t tt = std::chrono::system_clock::to_time_t(entry.timestamp);
                std::tm tm{};
#if defined(_WIN32)
                localtime_s(&tm, &tt);
#else
                localtime_r(&tt, &tm);
#endif
                std::ostringstream oss;
                oss << std::put_time(&tm, "%Y-%m-%d %H:%M");
                ImGui::TextUnformatted(oss.str().c_str());
                ImGui::TableSetColumnIndex(4);
                std::string buttonLabel = std::format("Unblock##{}", entry.id);
                if (ImGui::SmallButton(buttonLabel.c_str())) {
                    appModel.unblockPluginFromBlocklist(entry.id);
                }
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    }

    // Render the plugin list component
    pluginList_.render();

    // Plugin instantiation controls
    auto selection = pluginList_.getSelection();
    bool canInstantiate = selection.hasSelection;
    if (!canInstantiate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Instantiate Plugin")) {
        // Call the callback
        if (onInstantiatePlugin_) {
            onInstantiatePlugin_(selection.format, selection.pluginId, targetTrackIndex_);
        }
    }
    if (!canInstantiate) {
        ImGui::EndDisabled();
    }

    if (targetIsMasterTrack_) {
        ImGui::TextUnformatted("Destination: Master Track");
    } else if (targetTrackIndex_ < 0) {
        ImGui::TextUnformatted("Destination: New Track (new UMP device)");
        // Show device configuration for new track
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Device Name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("##device_name", deviceNameInput_, sizeof(deviceNameInput_));
        ImGui::SameLine();
        ImGui::TextUnformatted("API:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##api", apiInput_, sizeof(apiInput_));
    } else {
        ImGui::Text("Destination: Track %d", targetTrackIndex_ + 1);
    }

    if (reportWindowOpen_) {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scan Report", &reportWindowOpen_, ImGuiWindowFlags_NoSavedSettings)) {
            if (scanReportBuffer_.empty())
                refreshReportBuffer();
            auto parsedRows = parseScanReportMarkdown(scanReportText_);
            if (!parsedRows.empty()) {
                if (ImGui::Button("Copy as Markdown")) {
                    ImGui::SetClipboardText(scanReportText_.c_str());
                }
                ImGui::SameLine();
                ImGui::TextUnformatted("Table preview");

                if (ImGui::BeginTable("ScanReportTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Bundle", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    ImGui::TableSetupColumn("Scan Time (s)", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Plugins", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    ImGui::TableHeadersRow();
                    for (const auto& row : parsedRows) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextWrapped("%s", row.bundle.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(row.timeSeconds.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextWrapped("%s", row.plugins.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::Separator();
            }
            ImGui::TextDisabled("Markdown");
            ImVec2 textRegion = ImGui::GetContentRegionAvail();
            if (textRegion.x < 200.0f) textRegion.x = 200.0f;
            if (textRegion.y < 140.0f) textRegion.y = 140.0f;
            if (ImGui::BeginChild("ScanReportScrollRegion", textRegion, true, ImGuiWindowFlags_HorizontalScrollbar)) {
                const float minContentWidth = 1100.0f;
                ImVec2 textBoxSize(std::max(textRegion.x, minContentWidth),
                                   std::max(textRegion.y - ImGui::GetStyle().FramePadding.y * 2.0f, 100.0f));
                ImGui::InputTextMultiline("##scan_report_text",
                                          scanReportBuffer_.data(),
                                          scanReportBuffer_.size(),
                                          textBoxSize,
                                          ImGuiInputTextFlags_ReadOnly);
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
}

void PluginSelector::setPlugins(const std::vector<PluginEntry>& plugins) {
    pluginList_.setPlugins(plugins);
}

void PluginSelector::setOnInstantiatePlugin(std::function<void(const std::string& format, const std::string& pluginId, int32_t trackIndex)> callback) {
    onInstantiatePlugin_ = callback;
}

void PluginSelector::setOnScanPlugins(std::function<void(bool forceRescan, bool useRemoteProcess, double remoteTimeoutSeconds)> callback) {
    onScanPlugins_ = callback;
}

void PluginSelector::setScanning(bool scanning) {
    isScanning_ = scanning;
}

void PluginSelector::setTargetTrackIndex(int32_t trackIndex) {
    targetTrackIndex_ = trackIndex;
    targetIsMasterTrack_ = false;
}

void PluginSelector::setTargetNewTrack() {
    targetTrackIndex_ = -1;
    targetIsMasterTrack_ = false;
}

void PluginSelector::setTargetMasterTrack(int32_t masterTrackIndex) {
    targetTrackIndex_ = masterTrackIndex;
    targetIsMasterTrack_ = true;
}

void PluginSelector::refreshReportBuffer() {
    scanReportBuffer_.assign(scanReportText_.begin(), scanReportText_.end());
    scanReportBuffer_.push_back('\0');
}

}
