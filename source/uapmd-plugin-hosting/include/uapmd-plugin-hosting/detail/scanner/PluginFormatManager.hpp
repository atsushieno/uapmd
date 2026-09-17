#pragma once

#include <memory>
#include <vector>

#include "../plugin-api/AudioPluginFormat.hpp"

namespace uapmd_plugin_hosting {

// Holds the plugin formats this host knows about.
//
// The formats that are available on the current platform are created and owned here; an
// application adds one of its own with addFormat(), which does not take ownership.
class PluginFormatManager {
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    PluginFormatManager();
    ~PluginFormatManager();

    std::vector<AudioPluginFormat*> formats() const;
    const std::vector<AudioPluginFormat*>& formatView() const;
    // Adds an application-provided format. The caller keeps ownership and must keep the
    // format alive for as long as this manager is used.
    void addFormat(AudioPluginFormat* format);
};

} // namespace uapmd_plugin_hosting
