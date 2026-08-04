#pragma once

#include <string_view>

namespace uapmd::ara {

    inline constexpr std::string_view kAraPluginInstanceHandleExtensionId =
        "dev.atsushieno.uapmd-ara.plugin-instance.handles.v1";

    enum class AraPluginInstanceHandleKind {
        VST3Factory,
        VST3Component,
        VST3AudioProcessor,
        VST3Unknown,
        CLAPPlugin,
        AudioUnitV2,
        AudioUnitV3,
        AudioUnitV3BridgedV2
    };

    class AraPluginInstanceHandleExtension {
    public:
        virtual ~AraPluginInstanceHandleExtension() = default;
        virtual void* nativeHandle(AraPluginInstanceHandleKind kind) const = 0;
    };

} // namespace uapmd::ara
