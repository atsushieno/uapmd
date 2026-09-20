#include <array>

#include <uapmd-addin-core/uapmd-addin-core.hpp>
#include <uapmd-app-model/uapmd-app-model.hpp>

namespace uapmd_app {

// Registration and teardown run on the control thread, alongside plugin
// instantiation. MIDI/audio callbacks never enter the addin registry.
class VirtualMidiDevicesAddin final
    : public uapmd_addin::Addin
    , public uapmd_addin::Command
    , public uapmd::PluginInstanceLifecycleListener {
public:
    uapmd_addin::AddinIdentity identity() const noexcept override {
        return {"/uapmd/midi-service", "virtual-midi-devices"};
    }
    std::string_view name() const noexcept override { return "Virtual MIDI Devices"; }
    std::string_view path() const noexcept override {
        return "/uapmd/engine/plugin-instance-lifecycle/v1";
    }
    std::string_view id() const noexcept override { return "uapmd.virtual-midi-devices"; }
    std::string_view title() const noexcept override { return name(); }
    void invoke() noexcept override {
        if (model_ && model_->showVirtualMidiDevices)
            model_->showVirtualMidiDevices();
    }

    bool initialize(uapmd_addin::AddinHost& host) noexcept override {
        model_ = static_cast<AppModel*>(host.extensionPoint("/uapmd/app/model/v1"));
        if (!model_)
            return false;
        try {
            model_->register_virtual_midi_device_ = [this](int32_t instanceId) {
                if (!model_->autoCreateVirtualMidiDevices())
                    return;
                if (auto state = model_->getDeviceForInstance(instanceId))
                    model_->enableUmpDevice(instanceId, (*state)->label);
            };
            auto* engine = model_->sequencer().engine();
            engine->addPluginInstanceLifecycleListener(*this);
            if (auto* pluginHost = engine->pluginHost())
                for (auto instanceId : pluginHost->instanceIds()) {
                    model_->registerPluginInstanceInternal(instanceId, std::nullopt);
                    auto state = model_->getDeviceForInstance(instanceId);
                    if (model_->autoCreateVirtualMidiDevices() && state && !(*state)->device)
                        model_->enableUmpDevice(instanceId, (*state)->label);
                }
            commands_ = static_cast<uapmd_addin::CommandRegistry*>(
                host.extensionPoint("/uapmd/app/command/v1"));
            if (commands_)
                commands_->registerCommand(*this);
            return true;
        } catch (...) {
            cleanup(host);
            return false;
        }
    }

    void cleanup(uapmd_addin::AddinHost&) noexcept override {
        if (commands_)
            commands_->unregisterCommand(*this);
        commands_ = nullptr;
        if (!model_)
            return;
        auto* engine = model_->sequencer().engine();
        engine->removePluginInstanceLifecycleListener(*this);
        model_->register_virtual_midi_device_ = {};
        for (const auto& entry : model_->getDevices())
            if (entry.state && entry.state->device)
                for (const auto& [instanceId, state] : entry.state->pluginInstances)
                    model_->disableUmpDevice(instanceId);
        engine->functionBlockManager()->deleteEmptyDevices();
        model_ = nullptr;
    }

    void pluginInstanceAdded(int32_t instanceId,
                            uapmd_plugin_hosting::AudioPluginInstanceAPI&) override {
        model_->registerPluginInstanceInternal(instanceId, std::nullopt);
    }

    void pluginInstanceWillBeDestroyed(int32_t instanceId) override {
        if (auto state = model_->getDeviceForInstance(instanceId); state && (*state)->device)
            model_->disableUmpDevice(instanceId);
        // The app's completion callback owns UI removal notifications.
        std::lock_guard lock(model_->devicesMutex_);
        std::erase_if(model_->devices_, [instanceId](const auto& entry) {
            return entry.state && entry.state->pluginInstances.contains(instanceId);
        });
    }

private:
    AppModel* model_{};
    uapmd_addin::CommandRegistry* commands_{};
};

namespace {
class VirtualMidiDevicesEntry final : public uapmd_addin::AddinEntry {
public:
    std::string_view packageId() const noexcept override { return "/uapmd/midi-service"; }
    std::span<uapmd_addin::Addin* const> addins() noexcept override { return addins_; }
private:
    VirtualMidiDevicesAddin addin_;
    std::array<uapmd_addin::Addin*, 1> addins_{&addin_};
};
}

void registerVirtualMidiDevicesAddin() {
    static VirtualMidiDevicesEntry entry;
    static const bool registered = [] {
        uapmd_addin::registerBuiltinAddin(entry);
        return true;
    }();
    (void) registered;
}

} // namespace uapmd_app
