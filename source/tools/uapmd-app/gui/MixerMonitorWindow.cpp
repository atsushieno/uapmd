#include "MixerMonitorWindow.hpp"

#include <cmath>
#include <format>
#include <utility>

#include <uapmd-app-model/uapmd-app-model.hpp>

using namespace uapmd_graph;

namespace uapmd::gui {

namespace {

const char* inputMonitoringPolicyHint(uapmd::InputMonitoringPolicy policy) {
    switch (policy) {
    case uapmd::InputMonitoringPolicy::TAPE_STYLE:
        return "Prefer monitored live input over fully compensated playback on that path "
               "when the track is both record-armed and monitor-enabled.";
    case uapmd::InputMonitoringPolicy::OFF:
        return "Disable live-input monitoring through the latency-compensation layer.";
    case uapmd::InputMonitoringPolicy::AUTO:
    default:
        return "Use low-latency monitoring only for tracks that have live input and are "
               "explicitly monitor-enabled. Other playback remains compensated.";
    }
}

} // namespace

MixerMonitorWindow::MixerMonitorWindow(Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void MixerMonitorWindow::setCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void MixerMonitorWindow::toggle() {
    visible_ = !visible_;
}

void MixerMonitorWindow::hide() {
    visible_ = false;
}

void MixerMonitorWindow::render(float uiScale) {
    if (!visible_)
        return;

    const std::string windowId = "MixerMonitor";
    if (callbacks_.setChildSize)
        callbacks_.setChildSize(windowId, ImVec2(900.0f, 420.0f));

    if (!ImGui::Begin("Mixer Monitor", &visible_)) {
        ImGui::End();
        return;
    }

    if (callbacks_.updateChildSizeState)
        callbacks_.updateChildSizeState(windowId);

    auto& appModel = uapmd::AppModel::instance();
    auto* engine = appModel.sequencer().engine();
    if (!engine) {
        ImGui::TextDisabled("Sequencer engine is not available.");
        ImGui::End();
        return;
    }

    const auto audiblePosition = engine->playbackPosition();
    const auto renderPosition = engine->renderPlaybackPosition();
    const bool playbackActive = engine->isPlaybackActive();
    const bool prerollActive = playbackActive && renderPosition < audiblePosition;
    const bool latencyDrainActive = !playbackActive && renderPosition > audiblePosition;
    const bool outputAlignmentActive = engine->isOutputAlignmentActive();
    auto* latencyManager = engine->latencyCompensationManager();
    if (!latencyManager) {
        ImGui::TextDisabled("Latency compensation manager is not available.");
        ImGui::End();
        return;
    }
    int playbackCompMode = static_cast<int>(latencyManager->playbackCompensationMode());
    int inputMonitoringPolicy = static_cast<int>(latencyManager->inputMonitoringPolicy());
    int realtimeInfiniteTailPolicy = static_cast<int>(latencyManager->realtimeInfiniteTailPolicy());
    const char* playbackCompItems[] = {"Compensated", "Low-Latency"};
    const char* inputMonitoringItems[] = {"Tape Style", "Auto", "Off"};
    const char* realtimeInfiniteTailPolicyItems[] = {"Latency Fallback", "Immediate Stop"};

    ImGui::Text("Audible Position: %lld samples", static_cast<long long>(audiblePosition));
    ImGui::SameLine();
    ImGui::Text("Render Position: %lld samples", static_cast<long long>(renderPosition));
    ImGui::Text("Playback: %s", playbackActive ? "active" : "idle");
    ImGui::SameLine();
    ImGui::Text("Preroll: %s", prerollActive ? "active" : "inactive");
    ImGui::SameLine();
    ImGui::Text("Latency Drain: %s", latencyDrainActive ? "active" : "inactive");
    ImGui::SameLine();
    ImGui::Text("Output Alignment: %s", outputAlignmentActive ? "active" : "inactive");
    if (ImGui::Combo("Playback Compensation", &playbackCompMode, playbackCompItems, IM_ARRAYSIZE(playbackCompItems))) {
        latencyManager->playbackCompensationMode(static_cast<uapmd::PlaybackCompensationMode>(playbackCompMode));
        appModel.markProjectDirty();
    }
    if (ImGui::Combo("Input Monitoring", &inputMonitoringPolicy, inputMonitoringItems, IM_ARRAYSIZE(inputMonitoringItems))) {
        latencyManager->inputMonitoringPolicy(static_cast<uapmd::InputMonitoringPolicy>(inputMonitoringPolicy));
        appModel.markProjectDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", inputMonitoringPolicyHint(static_cast<uapmd::InputMonitoringPolicy>(inputMonitoringPolicy)));
    if (ImGui::Combo("DEBUG Realtime Infinite Tail", &realtimeInfiniteTailPolicy, realtimeInfiniteTailPolicyItems, IM_ARRAYSIZE(realtimeInfiniteTailPolicyItems)))
        latencyManager->realtimeInfiniteTailPolicy(static_cast<uapmd::RealtimeInfiniteTailPolicy>(realtimeInfiniteTailPolicy));

    ImGui::Separator();

    if (ImGui::BeginTable("MixerMonitorTable", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 110.0f * uiScale);
        ImGui::TableSetupColumn("Plugins", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Main Latency", ImGuiTableColumnFlags_WidthFixed, 95.0f * uiScale);
        ImGui::TableSetupColumn("Render Lead", ImGuiTableColumnFlags_WidthFixed, 95.0f * uiScale);
        ImGui::TableSetupColumn("Live Input", ImGuiTableColumnFlags_WidthFixed, 72.0f * uiScale);
        ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthFixed, 72.0f * uiScale);
        ImGui::TableSetupColumn("Monitor", ImGuiTableColumnFlags_WidthFixed, 76.0f * uiScale);
        ImGui::TableSetupColumn("Holdback", ImGuiTableColumnFlags_WidthFixed, 95.0f * uiScale);
        ImGui::TableSetupColumn("Tail", ImGuiTableColumnFlags_WidthFixed, 85.0f * uiScale);
        ImGui::TableSetupColumn("Out Buses", ImGuiTableColumnFlags_WidthFixed, 68.0f * uiScale);
        ImGui::TableSetupColumn("Per-Bus Timing / Route", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();

        auto renderTrackRow = [engine, latencyManager, &appModel](const char* label, int32_t trackIndex, uapmd::SequencerTrack* track, uint32_t mainLatency, uint32_t renderLead, uint32_t outputHoldback, double tailSeconds) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);

            ImGui::TableSetColumnIndex(1);
            std::string pluginSummary;
            if (track) {
                const auto& ids = track->orderedInstanceIds();
                for (size_t i = 0; i < ids.size(); ++i) {
                    auto* instance = engine->getPluginInstance(ids[i]);
                    if (!instance)
                        continue;
                    if (!pluginSummary.empty())
                        pluginSummary += " -> ";
                    pluginSummary += instance->displayName();
                }
            }
            if (pluginSummary.empty())
                pluginSummary = "(none)";
            ImGui::TextWrapped("%s", pluginSummary.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u samples", mainLatency);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u samples", renderLead);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted((trackIndex >= 0 && engine->trackHasLiveInput(static_cast<uapmd_track_index_t>(trackIndex))) ? "yes" : "no");

            ImGui::TableSetColumnIndex(5);
            if (trackIndex >= 0) {
                bool recordArmed = latencyManager->trackRecordArmed(static_cast<uapmd_track_index_t>(trackIndex));
                const std::string checkboxId = std::format("##record-track-{}", trackIndex);
                if (ImGui::Checkbox(checkboxId.c_str(), &recordArmed)) {
                    latencyManager->trackRecordArmed(static_cast<uapmd_track_index_t>(trackIndex), recordArmed);
                    appModel.markProjectDirty();
                }
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(6);
            if (trackIndex >= 0) {
                bool monitorEnabled = latencyManager->trackMonitoringEnabled(static_cast<uapmd_track_index_t>(trackIndex));
                const std::string checkboxId = std::format("##monitor-track-{}", trackIndex);
                if (ImGui::Checkbox(checkboxId.c_str(), &monitorEnabled)) {
                    latencyManager->trackMonitoringEnabled(static_cast<uapmd_track_index_t>(trackIndex), monitorEnabled);
                    appModel.markProjectDirty();
                }
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%u samples", outputHoldback);
            ImGui::TableSetColumnIndex(8);
            if (std::isinf(tailSeconds))
                ImGui::TextUnformatted("infinite");
            else
                ImGui::Text("%.3f s", tailSeconds);
            ImGui::TableSetColumnIndex(9);
            const uint32_t outputBusCount = track ? track->graph().outputBusCount() : 0;
            ImGui::Text("%u", outputBusCount);

            ImGui::TableSetColumnIndex(10);
            std::string busLatencySummary;
            if (track) {
                for (uint32_t busIndex = 0; busIndex < outputBusCount; ++busIndex) {
                    if (!busLatencySummary.empty())
                        busLatencySummary += ", ";
                    const uint32_t busLatency = track->graph().outputLatencyInSamples(busIndex);
                    const uint32_t busHoldback = trackIndex >= 0 ? engine->trackOutputBusAlignmentHoldbackInSamples(static_cast<uapmd_track_index_t>(trackIndex), busIndex) : 0;
                    std::string routeSummary = "route -";
                    if (trackIndex >= 0) {
                        const auto routeTarget = engine->trackOutputBusRoutingTarget(static_cast<uapmd_track_index_t>(trackIndex), busIndex);
                        switch (routeTarget.type) {
                        case TrackOutputRoutingTargetType::MASTER_INPUT_BUS:
                            routeSummary = std::format("master in {}", routeTarget.bus_index);
                            break;
                        case TrackOutputRoutingTargetType::MAIN_MIX_BUS:
                            routeSummary = std::format("main mix {}", routeTarget.bus_index);
                            break;
                        case TrackOutputRoutingTargetType::DISABLED:
                        default:
                            routeSummary = "disabled";
                            break;
                        }
                        if (routeTarget.folded)
                            routeSummary += " (folded)";
                    }
                    busLatencySummary += std::format("bus {}: lat {}, hold {}, {}", busIndex, busLatency, busHoldback, routeSummary);
                }
            }
            if (busLatencySummary.empty())
                busLatencySummary = "-";
            ImGui::TextWrapped("%s", busLatencySummary.c_str());
        };

        renderTrackRow("Master Track", kMasterTrackIndex, engine->masterTrack(), engine->masterTrackLatencyInSamples(), engine->masterTrackRenderLeadInSamples(), 0, engine->masterTrack() ? engine->masterTrack()->tailLengthInSeconds() : 0.0);

        const auto& tracks = engine->tracks();
        for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
            auto* track = tracks[trackIndex];
            const std::string label = std::format("Track {}", trackIndex);
            renderTrackRow(label.c_str(), static_cast<int32_t>(trackIndex), track, engine->trackLatencyInSamples(static_cast<uapmd_track_index_t>(trackIndex)), engine->trackRenderLeadInSamples(static_cast<uapmd_track_index_t>(trackIndex)), engine->trackOutputAlignmentHoldbackInSamples(static_cast<uapmd_track_index_t>(trackIndex)), track ? track->tailLengthInSeconds() : 0.0);
        }

        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace uapmd::gui
