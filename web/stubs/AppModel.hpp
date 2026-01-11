#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace uapmd {

class AudioPluginSequencer; // Provided by stub impl in sequencer_stubs.cpp

class AppModel {
public:
    struct PluginInstanceState {
        std::string pluginName;
        std::string pluginFormat;
        std::string pluginId;
        std::string statusMessage;
        bool instantiating = true;
        bool hasError = false;
        int32_t instanceId = -1;
    };

    struct DeviceState {
        std::mutex mutex;
        std::string label;
        std::string apiName;
        std::string statusMessage;
        bool running = false;
        bool instantiating = false;
        bool hasError = false;
        std::unordered_map<int32_t, PluginInstanceState> pluginInstances;
    };

    struct DeviceEntry { int id; std::shared_ptr<DeviceState> state; };

    struct PluginInstanceConfig {
        std::string apiName = "default";
        std::string deviceName;
        std::string manufacturer = "UAPMD Project";
        std::string version = "0.1";
    };

    struct PluginInstanceResult {
        int32_t instanceId = -1;
        std::shared_ptr<void> device; // unused in web stub
        std::string pluginName;
        std::string error;
    };

    struct DeviceStateResult {
        int32_t instanceId = -1;
        bool success = false;
        bool running = false;
        std::string statusMessage;
        std::string error;
    };

    struct UIStateResult {
        int32_t instanceId = -1;
        bool success = false;
        bool visible = false;
        bool wasCreated = false;
        void* uiHandle = nullptr;
        std::string error;
    };

    static void instantiate();
    static AppModel& instance();
    static void cleanupInstance();

    AppModel();

    AudioPluginSequencer& sequencer();

    void performPluginScanning(bool forceRescan = false);
    bool isScanning() const { return isScanning_; }

    void createPluginInstanceAsync(const std::string& format,
                                   const std::string& pluginId,
                                   int32_t trackIndex,
                                   const PluginInstanceConfig& config);
    void removePluginInstance(int32_t instanceId);

    std::vector<DeviceEntry> getDevices() const { return {}; }
    std::optional<std::shared_ptr<DeviceState>> getDeviceForInstance(int32_t) const { return std::nullopt; }
    void updateDeviceLabel(int32_t, const std::string&) {}

    void enableUmpDevice(int32_t, const std::string&) {}
    void disableUmpDevice(int32_t) {}

    void requestShowPluginUI(int32_t instanceId);
    void showPluginUI(int32_t instanceId, bool, bool, void*, std::function<bool(uint32_t,uint32_t)>) {
        // No plugin UI in web stub
        for (auto& cb : uiShown) cb(UIStateResult{ .instanceId = instanceId, .success = false, .visible = false });
    }
    void hidePluginUI(int32_t instanceId) {
        for (auto& cb : uiHidden) cb(UIStateResult{ .instanceId = instanceId, .success = true, .visible = false });
    }

    struct PluginStateResult { int32_t instanceId = -1; bool success = false; std::string error; std::string filepath; };
    PluginStateResult loadPluginState(int32_t instanceId, const std::string& filepath) { return { instanceId, false, "Not supported on web", filepath }; }
    PluginStateResult savePluginState(int32_t instanceId, const std::string& filepath) { return { instanceId, false, "Not supported on web", filepath }; }

    // Callbacks
    std::vector<std::function<void(bool, std::string)>> scanningCompleted{};
    std::vector<std::function<void(const PluginInstanceResult&)>> instanceCreated{};
    std::vector<std::function<void(int32_t)>> instanceRemoved{};
    std::vector<std::function<void(const DeviceStateResult&)>> deviceEnabled{};
    std::vector<std::function<void(const DeviceStateResult&)>> deviceDisabled{};
    std::vector<std::function<void(const UIStateResult&)>> uiShown{};
    std::vector<std::function<void(const UIStateResult&)>> uiHidden{};
    std::vector<std::function<void(int32_t)>> uiShowRequested{};

private:
    std::unique_ptr<AudioPluginSequencer> sequencer_;
    std::atomic<bool> isScanning_{false};
    int32_t nextInstanceId_{1};
};

}

