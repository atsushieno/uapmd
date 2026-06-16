#pragma once

#include "remidy/detail/plugin-instance.hpp"

namespace remidy {

    inline constexpr std::string_view kNativePluginInstanceHandleExtensionId =
        "dev.atsushieno.remidy.plugin-instance.native-handles.v1";

    enum class NativePluginInstanceHandleKind {
        VST3Factory,
        VST3Component,
        VST3AudioProcessor,
        VST3Unknown,
        CLAPPlugin,
        AudioUnitV2,
        AudioUnitV3,
        AudioUnitV3BridgedV2
    };

    class NativePluginInstanceHandleExtension : public PluginExtensibility<PluginInstance> {
    protected:
        explicit NativePluginInstanceHandleExtension(PluginInstance& owner)
            : PluginExtensibility(owner) {
        }

    public:
        virtual void* nativeHandle(NativePluginInstanceHandleKind kind) const = 0;
    };
}
