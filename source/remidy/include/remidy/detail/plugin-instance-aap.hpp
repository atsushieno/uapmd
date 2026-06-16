#pragma once

#include "remidy/detail/plugin-instance.hpp"

namespace remidy {

    inline constexpr std::string_view kAAPPluginInstanceExtensionId =
        "dev.atsushieno.remidy.aap.instance.v1";

    class PluginInstanceAAPExt : public PluginExtensibility<PluginInstance> {
    protected:
        explicit PluginInstanceAAPExt(PluginInstance& owner) : PluginExtensibility(owner) {}

    public:
        virtual const std::string& pluginPackageName() const = 0;
        virtual const std::string& pluginLocalName() const = 0;
        virtual int32_t instanceId() const = 0;
    };
}
