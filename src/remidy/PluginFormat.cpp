
#include "remidy.hpp"

namespace remidy {
    class PluginFormat::Impl {
        PluginFormat* owner;
        PluginCatalog cached_plugins{};
    public:
        explicit Impl(PluginFormat* owner);
        PluginCatalog& getAvailablePlugins();
    };

    PluginFormat::PluginFormat() {
        impl = new Impl(this);
    }

    bool PluginScanner::scanningMayBeSlow() {
        return scanRequiresInstantiation() != ScanningStrategyValue::NEVER;
    }

    PluginFormat::Impl::Impl(PluginFormat* owner) : owner(owner) {}
}
