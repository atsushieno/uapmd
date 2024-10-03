
#include "remidy.hpp"

namespace remidy {
    class AudioPluginFormat::Impl {
        AudioPluginFormat* owner;
        std::vector<PluginCatalogEntry*> cached_plugins{};
    public:
        explicit Impl(AudioPluginFormat* owner);
        bool hasPluginListCache();
        std::vector<PluginCatalogEntry*> getAvailablePlugins();
    };

    AudioPluginFormat::AudioPluginFormat() {
        impl = new Impl(this);
    }

    bool AudioPluginFormat::hasPluginListCache() {
        return impl->hasPluginListCache();
    }

    std::vector<PluginCatalogEntry*> AudioPluginFormat::getAvailablePlugins() {
        return impl->getAvailablePlugins();
    }

    AudioPluginFormat::Impl::Impl(AudioPluginFormat* owner) : owner(owner) {}

    bool AudioPluginFormat::Impl::hasPluginListCache() { return !cached_plugins.empty(); }

    std::vector<PluginCatalogEntry*> AudioPluginFormat::Impl::getAvailablePlugins() {
        if (cached_plugins.empty())
            cached_plugins.append_range(owner->scanAllAvailablePlugins());
        return cached_plugins;
    }
}
