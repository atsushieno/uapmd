#pragma once

#include <string_view>
#include "remidy/remidy.hpp"
#include "AudioPluginInstanceAPI.hpp"

// Bridges to remidy's AAP (Android Audio Plugin) instance extensibility.
// Only meaningful for the AAP format; other formats never register it.
namespace uapmd {
    inline constexpr std::string_view kAapUiHostDetailsExtensionId =
        "dev.atsushieno.uapmd.plugin-instance.aap-ui-host-details.v1";

    class AapUiHostDetailsExtension : public AudioPluginInstanceExtension {
    public:
        std::string_view extensionId() const override {
            return kAapUiHostDetailsExtensionId;
        }

        virtual remidy::PluginInstanceAAPExt* aapExtensibility() const = 0;
    };
}
