#include <array>
#include <set>

#include <uapmd-addin-core/uapmd-addin-core.hpp>
#include <uapmd-ara/uapmd-ara.hpp>

using namespace uapmd_addin;

namespace {

class AraAddin final
    : public Addin
    , public uapmd::PluginInstanceLifecycleListener {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/ara", "support"};
    }

    std::string_view name() const noexcept override {
        return "ARA support";
    }

    std::string_view path() const noexcept override {
        return "/uapmd/engine/plugin-instance-lifecycle/v1";
    }

    bool initialize(AddinHost& host) noexcept override {
        auto* engine = static_cast<uapmd::SequencerEngine*>(
            host.extensionPoint("/uapmd/engine/v1"));
        if (!engine)
            return false;

        try {
            support_ = uapmd::ara::createAraSupport(*engine);
            engine_ = engine;
            engine_->addPluginInstanceLifecycleListener(*this);
            if (auto* pluginHost = engine_->pluginHost()) {
                for (int32_t instanceId : pluginHost->instanceIds()) {
                    if (auto* instance = pluginHost->getInstance(instanceId))
                        pluginInstanceAdded(instanceId, *instance);
                }
            }
            return true;
        } catch (...) {
            cleanup(host);
            return false;
        }
    }

    void cleanup(AddinHost&) noexcept override {
        if (engine_)
            engine_->removePluginInstanceLifecycleListener(*this);
        if (support_) {
            for (int32_t instanceId : attached_instances_)
                support_->detachPlugin(instanceId);
        }
        attached_instances_.clear();
        support_.reset();
        engine_ = nullptr;
    }

    void pluginInstanceAdded(
        int32_t instanceId,
        uapmd_plugin_hosting::AudioPluginInstanceAPI& instance) override {
        if (!support_)
            return;
        if (support_->attachPlugin(instanceId, instance) == uapmd::ara::AraStatus::Ok)
            attached_instances_.insert(instanceId);
    }

    void pluginInstanceWillBeDestroyed(int32_t instanceId) override {
        if (support_ && attached_instances_.erase(instanceId) != 0)
            support_->detachPlugin(instanceId);
    }

private:
    uapmd::SequencerEngine* engine_{};
    std::unique_ptr<uapmd::ara::AraSupport> support_;
    std::set<int32_t> attached_instances_;
};

class AraAddinEntry final : public AddinEntry {
public:
    AraAddinEntry() {
        addins_[0] = &addin_;
    }

    std::string_view packageId() const noexcept override {
        return "/uapmd/ara";
    }

    std::span<Addin* const> addins() noexcept override {
        return addins_;
    }

private:
    AraAddin addin_;
    std::array<Addin*, 1> addins_{};
};

AraAddinEntry araAddinEntry;

class AraBuiltinAddinRegistration final {
public:
    AraBuiltinAddinRegistration() {
        registerBuiltinAddin(araAddinEntry);
    }
};

AraBuiltinAddinRegistration araBuiltinAddinRegistration;

} // namespace
