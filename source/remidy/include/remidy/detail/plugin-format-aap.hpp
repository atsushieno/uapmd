#pragma once

#include "remidy/detail/plugin-format.hpp"

namespace remidy {

    class PluginFormatAAP : public PluginFormat {
    public:
        PluginFormatAAP() = default;

        std::string name() override { return "AAP"; }

        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }

        bool canOmitUiState() override { return true; }

        bool isStateStructured() override { return false; }

        static std::unique_ptr<PluginFormatAAP> create();
    };
}