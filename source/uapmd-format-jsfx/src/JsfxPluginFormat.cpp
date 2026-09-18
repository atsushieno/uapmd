#include "uapmd-format-jsfx/uapmd-format-jsfx.hpp"

#if UAPMD_JSFX_OWN_FONTS
#include "JsfxFontBackend.hpp"
#endif
#include "JsfxPluginInstance.hpp"
#include "JsfxScanning.hpp"

namespace uapmd_jsfx {

    void setJsfxEditorFontData(const void* data, size_t size) {
#if UAPMD_JSFX_OWN_FONTS
        setJsfxFontData(data, size);
#else
        // The platform's own fonts are in use; there is nothing to supply.
        (void) data;
        (void) size;
#endif
    }

    bool jsfxUsesOwnFontBackend() {
#if UAPMD_JSFX_OWN_FONTS
        return true;
#else
        return false;
#endif
    }

    struct JsfxPluginFormat::Impl {
        JsfxScanning scanning{};
    };

    JsfxPluginFormat::JsfxPluginFormat() : impl_(std::make_unique<Impl>()) {
    }

    JsfxPluginFormat::~JsfxPluginFormat() = default;

    std::string JsfxPluginFormat::name() {
        return kFormatName;
    }

    uapmd_plugin_hosting::AudioPluginUIThreadRequirement JsfxPluginFormat::requiresUIThreadOn(
            uapmd_plugin_hosting::AudioPluginCatalogEntry* entry) {
        (void) entry;
        return uapmd_plugin_hosting::UIThreadNotRequired;
    }

    uapmd_plugin_hosting::AudioPluginScanning* JsfxPluginFormat::scanning() {
        return &impl_->scanning;
    }

    void JsfxPluginFormat::createInstance(
            uapmd_plugin_hosting::AudioPluginCatalogEntry* entry,
            const uapmd_plugin_hosting::AudioPluginInstantiationOptions& options,
            std::function<void(std::unique_ptr<uapmd_plugin_hosting::AudioPluginInstanceAPI> instance,
                               std::string error)> callback) {
        if (!callback)
            return;
        if (!entry) {
            callback(nullptr, "no plugin was named");
            return;
        }

        // The catalog remembers where the effect was when it was scanned, but a project
        // may outlive that: search paths change, and effects move between machines. The
        // plugin id is the portable name, so it is what we resolve against the current
        // search paths, falling back to the remembered location.
        auto path = impl_->scanning.resolvePluginId(entry->pluginId());
        if (!path) {
            auto& remembered = entry->bundlePath();
            std::error_code ec{};
            if (!remembered.empty() && std::filesystem::is_regular_file(remembered, ec))
                path = remembered;
        }
        if (!path) {
            callback(nullptr, "could not find the JSFX effect " + entry->pluginId());
            return;
        }

        // JSFX has no main thread affinity and compiling a script is not slow, so this
        // completes before returning rather than deferring to a loader thread.
        std::string error{};
        auto instance = JsfxPluginInstance::create(*path, entry->pluginId(), options, error);
        if (!instance) {
            callback(nullptr, error.empty() ? "could not load the JSFX effect" : error);
            return;
        }
        callback(std::move(instance), "");
    }

    void JsfxPluginFormat::searchPaths(std::vector<std::string> paths, bool alsoUseDefaults) {
        auto& overrides = impl_->scanning.getOverrideSearchPaths();
        overrides.clear();
        for (auto& path : paths)
            overrides.emplace_back(std::move(path));
        impl_->scanning.useDefaultSearchPaths(alsoUseDefaults);
    }

    std::vector<std::string> JsfxPluginFormat::searchPaths() const {
        return impl_->scanning.getOverrideSearchPaths();
    }

    bool JsfxPluginFormat::useDefaultSearchPaths() const {
        return impl_->scanning.useDefaultSearchPaths();
    }

    std::vector<std::filesystem::path> JsfxPluginFormat::defaultSearchPaths() const {
        return impl_->scanning.getDefaultSearchPaths();
    }

}
