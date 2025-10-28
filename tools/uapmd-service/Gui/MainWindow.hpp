#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <remidy-tooling/PluginScanTool.hpp>
#include <remidy-gui/ContainerWindow.hpp>

#include "../Controller/VirtualMidiDeviceController.hpp"

namespace uapmd::service::gui {

struct GuiDefaults {
    std::string pluginName;
    std::string formatName;
    std::string apiName;
    std::string deviceName;
};

class MainWindow {
public:
    explicit MainWindow(VirtualMidiDeviceController& controller, GuiDefaults defaults);
    ~MainWindow();

    void render();
    void update();

    bool isOpen() const { return isOpen_; }
    void close() { isOpen_ = false; }

    void shutdown();

private:
    struct PluginEntry {
        std::string format;
        std::string pluginId;
        std::string displayName;
        std::string vendor;
    };

    struct PluginInstanceState {
        std::string pluginName;
        std::string pluginFormat;
        std::string pluginId;
        std::string statusMessage;
        bool instantiating = true;
        bool hasError = false;
        int32_t instanceId = -1;
        int32_t trackIndex = -1;

        std::unique_ptr<remidy::gui::ContainerWindow> pluginWindow;
        bool pluginWindowEmbedded = false;
        remidy::gui::Bounds pluginWindowBounds{100, 100, 800, 600};
        bool pluginWindowResizeIgnore = false;
    };

    struct DeviceState {
        std::mutex mutex;
        std::shared_ptr<UapmdMidiDevice> device;
        std::string label;
        std::string apiName;
        std::string statusMessage;
        bool running = false;
        bool instantiating = false;
        bool hasError = false;

        std::unordered_map<int32_t, PluginInstanceState> pluginInstances;
    };

    struct DeviceEntry {
        int id;
        std::shared_ptr<DeviceState> state;
    };

    struct TrackDestinationOption {
        int deviceEntryId;
        int32_t trackIndex;
        std::string label;
    };

    void startPluginScan(bool forceRescan);
    void finalizePluginScan(std::vector<PluginEntry>&& entries, int scanResult, const std::string& errorMessage);
    void renderPluginSelector();
    void renderDeviceManager();
    void createDeviceForPlugin(size_t pluginIndex, const TrackDestinationOption* destination);
    void removeDevice(size_t index);
    void attemptDefaultDeviceCreation();
    void stopAllDevices();

    std::vector<int> filteredPluginIndices(const std::vector<PluginEntry>& plugins) const;

    // Plugin UI helper methods
    bool handlePluginResizeRequest(std::shared_ptr<DeviceState> state, int32_t instanceId, uint32_t width, uint32_t height);
    void onPluginWindowResized(std::shared_ptr<DeviceState> state, int32_t instanceId);
    void onPluginWindowClosed(std::shared_ptr<DeviceState> state, int32_t instanceId);
    void showPluginUIInstance(std::shared_ptr<DeviceState> state, int32_t instanceId);
    void hidePluginUIInstance(std::shared_ptr<DeviceState> state, int32_t instanceId);

    PluginInstanceState* findPluginInstance(DeviceState& state, int32_t instanceId);
    std::shared_ptr<DeviceState> findDeviceById(int deviceEntryId);

    VirtualMidiDeviceController& controller_;
    GuiDefaults defaults_;

    bool isOpen_ = true;

    std::atomic<bool> scanning_{false};
    std::atomic<bool> rescanRequested_{false};
    std::thread scanningThread_;
    mutable std::mutex pluginMutex_;
    std::vector<PluginEntry> plugins_;
    int selectedPlugin_ = -1;
    bool pluginScanCompleted_ = false;
    std::string pluginScanMessage_;

    mutable std::mutex devicesMutex_;
    std::vector<DeviceEntry> devices_;
    int nextDeviceId_ = 1;

    bool pendingDefaultDevice_ = false;
    bool defaultDeviceAttempted_ = false;

        std::array<char, 128> pluginFilter_{};
        std::array<char, 64> apiInput_{};
        std::array<char, 128> deviceNameInput_{};
        std::string selectedPluginFormat_;
        std::string selectedPluginId_;
        int selectedTrackOption_ = 0;
        std::vector<TrackDestinationOption> trackOptions_;

        static constexpr const char* defaultManufacturer_ = "UAPMD Project";
        static constexpr const char* defaultVersion_ = "0.1";
};

} // namespace uapmd::service::gui
