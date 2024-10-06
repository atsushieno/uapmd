
#include "remidy.hpp"

namespace remidy {
    class AudioPluginFormat::Impl {
        AudioPluginFormat* owner;
        PluginCatalog cached_plugins{};
    public:
        explicit Impl(AudioPluginFormat* owner);
        PluginCatalog& getAvailablePlugins();
    };

    AudioPluginFormat::AudioPluginFormat() {
        impl = new Impl(this);
    }

    bool AudioPluginFormat::hasPluginListCache() {
        return scanRequiresInstantiation() != ScanningStrategyValue::NO;
    }

    PluginCatalog& AudioPluginFormat::getAvailablePlugins() {
        return impl->getAvailablePlugins();
    }

    AudioPluginFormat::Impl::Impl(AudioPluginFormat* owner) : owner(owner) {}

    PluginCatalog& AudioPluginFormat::Impl::getAvailablePlugins() {
        if (!owner->hasPluginListCache() || cached_plugins.getPlugins().empty())
            cached_plugins = owner->scanAllAvailablePlugins();
        return cached_plugins;
    }
}
