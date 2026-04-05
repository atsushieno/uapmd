#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imnodes.h>
#include <uapmd/uapmd.hpp>

namespace uapmd::gui {

class PluginGraphEditor {
public:
    PluginGraphEditor();
    ~PluginGraphEditor();

    void showTrack(int32_t trackIndex);
    void showMasterTrack();
    void hideTrack(int32_t trackIndex);
    void hideMasterTrack();
    void hide();
    bool isVisible(int32_t trackIndex) const;
    bool visible() const { return !windows_.empty(); }
    void render(float uiScale,
                const std::function<void(const std::string&, ImVec2)>& setNextChildWindowSize,
                const std::function<void(const std::string&)>& updateChildWindowSizeState);

private:
    struct PinDescriptor {
        int32_t track_index{0};
        AudioPluginGraphEndpoint endpoint{};
        AudioPluginGraphBusType bus_type{AudioPluginGraphBusType::Audio};
        bool is_input{false};
    };

    struct WindowState {
        int32_t track_index{0};
        ImNodesContext* context{nullptr};
        ImNodesEditorContext* editor{nullptr};
        bool request_focus{false};
        std::unordered_set<int64_t> initialized_node_keys{};
        std::unordered_map<int64_t, int> node_ids{};
        std::unordered_map<int64_t, int> pin_ids{};
        std::unordered_map<int64_t, int> link_ids{};
        int next_node_id{1};
        int next_pin_id{1};
        int next_link_id{1};
    };

    std::unordered_map<int32_t, WindowState> windows_{};
    std::vector<int32_t> window_order_{};

    WindowState& ensureWindow(int32_t trackIndex);
    void destroyWindow(int32_t trackIndex);
    int64_t nodeKeyForTrackEndpoint(int32_t trackIndex, AudioPluginGraphEndpointType type, int32_t instanceId) const;
    int64_t pinKeyForDescriptor(const PinDescriptor& descriptor) const;
    int64_t linkKey(int32_t trackIndex, int64_t connectionId) const;
    int nodeIdForTrackEndpoint(WindowState& window, AudioPluginGraphEndpointType type, int32_t instanceId);
    int pinIdForDescriptor(WindowState& window, const PinDescriptor& descriptor);
    int linkIdForConnection(WindowState& window, int64_t connectionId);
    std::string windowId(int32_t trackIndex) const;
    std::string windowTitle(int32_t trackIndex) const;
    void renderGraph(WindowState& window, float uiScale);
};

}
