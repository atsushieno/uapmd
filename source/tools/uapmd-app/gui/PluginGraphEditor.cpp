#include "PluginGraphEditor.hpp"

#include <algorithm>
#include <cfloat>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "../AppModel.hpp"

namespace uapmd::gui {

namespace {

constexpr int32_t kMasterEditorTrack = uapmd::kMasterTrackIndex;

struct TrackAccess {
    SequencerTrack* track{nullptr};
    std::string label;
};

TrackAccess getTrackAccess(int32_t trackIndex) {
    auto& sequencer = AppModel::instance().sequencer();
    if (trackIndex == kMasterEditorTrack)
        return {sequencer.engine()->masterTrack(), "Master Plugin Graph"};
    auto tracks = sequencer.engine()->tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
        return {};
    return {tracks[static_cast<size_t>(trackIndex)], std::format("Track {} Plugin Graph", trackIndex)};
}

const char* endpointLabel(AudioPluginGraphBusType busType, bool isInput, uint32_t busIndex) {
    if (busType == AudioPluginGraphBusType::Event)
        return isInput ? "Event In" : "Event Out";
    return isInput ? "Audio In" : "Audio Out";
}

} // namespace

PluginGraphEditor::PluginGraphEditor() {
}

PluginGraphEditor::~PluginGraphEditor() {
    hide();
}

namespace {

int allocateStableId(std::unordered_map<int64_t, int>& ids, int64_t key, int& nextId) {
    if (auto it = ids.find(key); it != ids.end())
        return it->second;
    auto [it, _] = ids.emplace(key, nextId++);
    return it->second;
}

} // namespace

PluginGraphEditor::WindowState& PluginGraphEditor::ensureWindow(int32_t trackIndex) {
    if (auto it = windows_.find(trackIndex); it != windows_.end())
        return it->second;

    WindowState window;
    window.track_index = trackIndex;
    window.context = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(window.context);
    ImNodes::StyleColorsDark();
    auto& style = ImNodes::GetStyle();
    style.Flags |= ImNodesStyleFlags_GridLines;
    style.Flags |= ImNodesStyleFlags_NodeOutline;
    window.editor = ImNodes::EditorContextCreate();
    ImNodes::SetCurrentContext(nullptr);

    auto [it, _] = windows_.emplace(trackIndex, std::move(window));
    window_order_.push_back(trackIndex);
    return it->second;
}

void PluginGraphEditor::destroyWindow(int32_t trackIndex) {
    auto it = windows_.find(trackIndex);
    if (it == windows_.end())
        return;

    if (it->second.editor)
        ImNodes::EditorContextFree(it->second.editor);
    if (it->second.context)
        ImNodes::DestroyContext(it->second.context);
    windows_.erase(it);
    std::erase(window_order_, trackIndex);
}

int PluginGraphEditor::nodeIdForTrackEndpoint(WindowState& window, AudioPluginGraphEndpointType type, int32_t instanceId) {
    return allocateStableId(window.node_ids,
                            nodeKeyForTrackEndpoint(window.track_index, type, instanceId),
                            window.next_node_id);
}

int PluginGraphEditor::pinIdForDescriptor(WindowState& window, const PinDescriptor& descriptor) {
    return allocateStableId(window.pin_ids, pinKeyForDescriptor(descriptor), window.next_pin_id);
}

int PluginGraphEditor::linkIdForConnection(WindowState& window, int64_t connectionId) {
    return allocateStableId(window.link_ids, linkKey(window.track_index, connectionId), window.next_link_id);
}

void PluginGraphEditor::showTrack(int32_t trackIndex) {
    if (!AppModel::instance().ensureTrackUsesEditorGraph(trackIndex))
        return;
    auto& window = ensureWindow(trackIndex);
    window.request_focus = true;
}

void PluginGraphEditor::showMasterTrack() {
    showTrack(kMasterEditorTrack);
}

void PluginGraphEditor::hideTrack(int32_t trackIndex) {
    destroyWindow(trackIndex);
}

void PluginGraphEditor::hideMasterTrack() {
    hideTrack(kMasterEditorTrack);
}

void PluginGraphEditor::hide() {
    auto windowOrder = window_order_;
    for (int32_t trackIndex : windowOrder)
        destroyWindow(trackIndex);
}

bool PluginGraphEditor::isVisible(int32_t trackIndex) const {
    return windows_.contains(trackIndex);
}

int64_t PluginGraphEditor::nodeKeyForTrackEndpoint(
    int32_t trackIndex,
    AudioPluginGraphEndpointType type,
    int32_t instanceId
) const {
    const int64_t trackKey = static_cast<int64_t>(trackIndex == kMasterEditorTrack ? 1000000 : trackIndex + 1);
    const int64_t typeKey = type == AudioPluginGraphEndpointType::GraphInput ? 1 :
        type == AudioPluginGraphEndpointType::GraphOutput ? 2 : 3;
    const int64_t instanceKey = type == AudioPluginGraphEndpointType::Plugin ? static_cast<int64_t>(instanceId + 1) : 0;
    return (trackKey << 40) | (typeKey << 32) | instanceKey;
}

int64_t PluginGraphEditor::pinKeyForDescriptor(const PinDescriptor& descriptor) const {
    const int64_t nodeKey = nodeKeyForTrackEndpoint(descriptor.track_index,
                                                    descriptor.endpoint.type,
                                                    descriptor.endpoint.instance_id);
    const int64_t busTypeKey = descriptor.bus_type == AudioPluginGraphBusType::Audio ? 1 : 2;
    const int64_t dirKey = descriptor.is_input ? 1 : 2;
    return (nodeKey << 8) ^ (busTypeKey << 5) ^ (dirKey << 3) ^ static_cast<int64_t>(descriptor.endpoint.bus_index + 1);
}

int64_t PluginGraphEditor::linkKey(int32_t trackIndex, int64_t connectionId) const {
    return (static_cast<int64_t>(trackIndex == kMasterEditorTrack ? 1000000 : trackIndex + 1) << 40) |
        (connectionId & 0xFFFFFFFFFFLL);
}

std::string PluginGraphEditor::windowId(int32_t trackIndex) const {
    if (trackIndex == kMasterEditorTrack)
        return "PluginGraphMaster";
    return std::format("PluginGraphTrack{}", trackIndex);
}

std::string PluginGraphEditor::windowTitle(int32_t trackIndex) const {
    auto access = getTrackAccess(trackIndex);
    if (access.label.empty())
        return std::format("Plugin Graph##{}", windowId(trackIndex));
    return std::format("{}##{}", access.label, windowId(trackIndex));
}

void PluginGraphEditor::render(
    float uiScale,
    const std::function<void(const std::string&, ImVec2)>& setNextChildWindowSize,
    const std::function<void(const std::string&)>& updateChildWindowSizeState
) {
    auto windowOrder = window_order_;
    for (int32_t trackIndex : windowOrder) {
        auto it = windows_.find(trackIndex);
        if (it == windows_.end())
            continue;

        auto& window = it->second;
        const std::string id = windowId(trackIndex);
        if (setNextChildWindowSize)
            setNextChildWindowSize(id, ImVec2(480.0f, 360.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 240.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (window.request_focus) {
            ImGui::SetNextWindowFocus();
            window.request_focus = false;
        }

        bool open = true;
        if (ImGui::Begin(windowTitle(trackIndex).c_str(), &open)) {
            if (updateChildWindowSizeState)
                updateChildWindowSizeState(id);
            renderGraph(window, uiScale);
        }
        ImGui::End();

        if (!open)
            destroyWindow(trackIndex);
    }
}

void PluginGraphEditor::renderGraph(WindowState& window, float uiScale) {
    auto access = getTrackAccess(window.track_index);
    if (!access.track || !window.context || !window.editor) {
        ImGui::TextDisabled("Track unavailable.");
        return;
    }

    if (ImGui::Button("Revert to Simple Graph")) {
        if (AppModel::instance().revertTrackToSimpleGraph(window.track_index)) {
            destroyWindow(window.track_index);
            return;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Return this track to the simple linear graph.");
    ImGui::Separator();

    auto& graph = access.track->graph();
    auto* fullGraph = dynamic_cast<AudioPluginFullDAGraph*>(&graph);
    if (!fullGraph) {
        ImGui::TextDisabled("Advanced DAG editor is unavailable for this graph type.");
        return;
    }
    auto plugins = graph.plugins();
    auto connections = fullGraph->connections();
    auto* layoutExtension = graph.getExtension<AudioBusesLayoutExtension>();
    auto layout = layoutExtension ? layoutExtension->busesLayout() : AudioGraphBusesLayout{};

    ImNodes::SetCurrentContext(window.context);
    ImNodes::EditorContextSet(window.editor);

    std::unordered_map<int, PinDescriptor> pins;
    std::unordered_map<int, int64_t> links;
    auto registerPin = [&](const PinDescriptor& descriptor) {
        const auto pinId = pinIdForDescriptor(window, descriptor);
        pins.emplace(pinId, descriptor);
        return pinId;
    };

    ImNodes::BeginNodeEditor();

    auto renderEndpointNode = [&](AudioPluginGraphEndpointType type, const char* titleText) {
        const auto nodeId = nodeIdForTrackEndpoint(window, type, -1);
        ImNodes::BeginNode(nodeId);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(titleText);
        ImNodes::EndNodeTitleBar();
        const uint32_t audioBusCount = type == AudioPluginGraphEndpointType::GraphInput
            ? layout.audio_input_bus_count
            : layout.audio_output_bus_count;
        const uint32_t eventBusCount = type == AudioPluginGraphEndpointType::GraphInput
            ? layout.event_input_bus_count
            : layout.event_output_bus_count;

        for (uint32_t bus = 0; bus < audioBusCount; ++bus) {
            PinDescriptor descriptor{
                window.track_index,
                AudioPluginGraphEndpoint{type, -1, bus},
                AudioPluginGraphBusType::Audio,
                type == AudioPluginGraphEndpointType::GraphOutput
            };
            if (descriptor.is_input)
                ImNodes::BeginInputAttribute(registerPin(descriptor));
            else
                ImNodes::BeginOutputAttribute(registerPin(descriptor));
            ImGui::Text("%s %u", endpointLabel(descriptor.bus_type, descriptor.is_input, bus), bus);
            if (descriptor.is_input)
                ImNodes::EndInputAttribute();
            else
                ImNodes::EndOutputAttribute();
        }
        for (uint32_t bus = 0; bus < eventBusCount; ++bus) {
            PinDescriptor descriptor{
                window.track_index,
                AudioPluginGraphEndpoint{type, -1, bus},
                AudioPluginGraphBusType::Event,
                type == AudioPluginGraphEndpointType::GraphOutput
            };
            if (descriptor.is_input)
                ImNodes::BeginInputAttribute(registerPin(descriptor));
            else
                ImNodes::BeginOutputAttribute(registerPin(descriptor));
            ImGui::Text("%s %u", endpointLabel(descriptor.bus_type, descriptor.is_input, bus), bus);
            if (descriptor.is_input)
                ImNodes::EndInputAttribute();
            else
                ImNodes::EndOutputAttribute();
        }
        ImNodes::EndNode();

        const int64_t initKey = nodeKeyForTrackEndpoint(window.track_index, type, -1);
        if (!window.initialized_node_keys.contains(initKey)) {
            ImNodes::SetNodeGridSpacePos(nodeId, type == AudioPluginGraphEndpointType::GraphInput
                ? ImVec2(24.0f * uiScale, 96.0f * uiScale)
                : ImVec2(420.0f * uiScale, 96.0f * uiScale));
            window.initialized_node_keys.insert(initKey);
        }
    };

    renderEndpointNode(AudioPluginGraphEndpointType::GraphInput, "Graph Input");
    renderEndpointNode(AudioPluginGraphEndpointType::GraphOutput, "Graph Output");

    int pluginOrder = 0;
    for (const auto& [instanceId, node] : plugins) {
        if (!node || !node->instance())
            continue;
        auto* instance = node->instance();
        const auto nodeId = nodeIdForTrackEndpoint(window, AudioPluginGraphEndpointType::Plugin, instanceId);
        ImNodes::BeginNode(nodeId);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(instance->displayName().c_str());
        ImNodes::EndNodeTitleBar();

        const uint32_t eventInputCount = instance->audioBuses() && instance->audioBuses()->hasEventInputs() ? 1u : 0u;
        for (uint32_t bus = 0; bus < eventInputCount; ++bus) {
            PinDescriptor descriptor{
                window.track_index,
                AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, instanceId, bus},
                AudioPluginGraphBusType::Event,
                true
            };
            ImNodes::BeginInputAttribute(registerPin(descriptor));
            ImGui::Text("Event In %u", bus);
            ImNodes::EndInputAttribute();
        }

        if (instance->audioBuses()) {
            uint32_t bus = 0;
            for (auto* inputBus : instance->audioBuses()->audioInputBuses()) {
                if (!inputBus || !inputBus->enabled())
                    continue;
                PinDescriptor descriptor{
                    window.track_index,
                    AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, instanceId, bus},
                    AudioPluginGraphBusType::Audio,
                    true
                };
                ImNodes::BeginInputAttribute(registerPin(descriptor));
                ImGui::Text("Audio In %u", bus);
                ImNodes::EndInputAttribute();
                ++bus;
            }

            bus = 0;
            for (auto* outputBus : instance->audioBuses()->audioOutputBuses()) {
                if (!outputBus || !outputBus->enabled())
                    continue;
                PinDescriptor descriptor{
                    window.track_index,
                    AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, instanceId, bus},
                    AudioPluginGraphBusType::Audio,
                    false
                };
                ImNodes::BeginOutputAttribute(registerPin(descriptor));
                ImGui::Text("Audio Out %u", bus);
                ImNodes::EndOutputAttribute();
                ++bus;
            }

            const uint32_t eventOutputCount = instance->audioBuses()->hasEventOutputs() ? 1u : 0u;
            for (uint32_t eventBus = 0; eventBus < eventOutputCount; ++eventBus) {
                PinDescriptor descriptor{
                    window.track_index,
                    AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, instanceId, eventBus},
                    AudioPluginGraphBusType::Event,
                    false
                };
                ImNodes::BeginOutputAttribute(registerPin(descriptor));
                ImGui::Text("Event Out %u", eventBus);
                ImNodes::EndOutputAttribute();
            }
        }

        ImNodes::EndNode();

        const int64_t initKey = nodeKeyForTrackEndpoint(window.track_index,
                                                        AudioPluginGraphEndpointType::Plugin,
                                                        instanceId);
        if (!window.initialized_node_keys.contains(initKey)) {
            ImNodes::SetNodeGridSpacePos(nodeId, ImVec2(180.0f * uiScale, (80.0f + pluginOrder * 170.0f) * uiScale));
            window.initialized_node_keys.insert(initKey);
        }
        ++pluginOrder;
    }

    for (const auto& connection : connections) {
        PinDescriptor sourceDescriptor{
            window.track_index,
            connection.source,
            connection.bus_type,
            false
        };
        PinDescriptor targetDescriptor{
            window.track_index,
            connection.target,
            connection.bus_type,
            true
        };
        const auto linkId = linkIdForConnection(window, connection.id);
        links.emplace(linkId, connection.id);
        ImNodes::Link(linkId,
                      pinIdForDescriptor(window, sourceDescriptor),
                      pinIdForDescriptor(window, targetDescriptor));
    }

    ImNodes::MiniMap(0.18f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();

    int startPinId = 0;
    int endPinId = 0;
    if (ImNodes::IsLinkCreated(&startPinId, &endPinId)) {
        auto startIt = pins.find(startPinId);
        auto endIt = pins.find(endPinId);
        if (startIt != pins.end() && endIt != pins.end()) {
            auto source = startIt->second;
            auto target = endIt->second;
            if (source.is_input && !target.is_input)
                std::swap(source, target);
            if (!source.is_input && target.is_input && source.bus_type == target.bus_type) {
                fullGraph->connect(AudioPluginGraphConnection{
                    .id = 0,
                    .bus_type = source.bus_type,
                    .source = source.endpoint,
                    .target = target.endpoint,
                });
            }
        }
    }

    int hoveredLinkId = 0;
    if (ImNodes::IsLinkHovered(&hoveredLinkId) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        auto it = links.find(hoveredLinkId);
        if (it != links.end()) {
            fullGraph->disconnect(it->second);
            ImNodes::ClearLinkSelection(hoveredLinkId);
        }
    }

    int destroyedLinkId = 0;
    if (ImNodes::IsLinkDestroyed(&destroyedLinkId)) {
        auto it = links.find(destroyedLinkId);
        if (it != links.end())
            fullGraph->disconnect(it->second);
    }

    ImNodes::EditorContextSet(nullptr);
    ImNodes::SetCurrentContext(nullptr);
}

}
