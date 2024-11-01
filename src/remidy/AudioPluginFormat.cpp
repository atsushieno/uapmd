
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

    bool AudioPluginScanner::scanningMayBeSlow() {
        return scanRequiresInstantiation() != ScanningStrategyValue::NEVER;
    }

    AudioPluginFormat::Impl::Impl(AudioPluginFormat* owner) : owner(owner) {}
}
