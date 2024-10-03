
#include "remidy.hpp"

namespace remidy {
    class AudioPluginFormat::Impl {
        AudioPluginFormat* owner;
        PluginCatalog cached_plugins{};
    public:
        explicit Impl(AudioPluginFormat* owner);
        bool hasPluginListCache();
        PluginCatalog& getAvailablePlugins();
    };

    AudioPluginFormat::AudioPluginFormat() {
        impl = new Impl(this);
    }

    bool AudioPluginFormat::hasPluginListCache() {
        return impl->hasPluginListCache();
    }

    PluginCatalog& AudioPluginFormat::getAvailablePlugins() {
        return impl->getAvailablePlugins();
    }

    AudioPluginFormat::Impl::Impl(AudioPluginFormat* owner) : owner(owner) {}

    bool AudioPluginFormat::Impl::hasPluginListCache() { return !cached_plugins.getPlugins().empty(); }

    PluginCatalog& AudioPluginFormat::Impl::getAvailablePlugins() {
        if (cached_plugins.getPlugins().empty())
            cached_plugins = owner->scanAllAvailablePlugins();
        return cached_plugins;
    }
}
