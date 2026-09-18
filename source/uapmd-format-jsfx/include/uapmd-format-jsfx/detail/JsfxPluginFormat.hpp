#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_jsfx {

    // Gives the JSFX editor a font to draw with.
    //
    // On Windows, macOS and Linux the editor uses the platform's own fonts, exactly as
    // REAPER does, and this does nothing. Elsewhere there is no platform font stack to
    // borrow, so the application supplies TrueType data of its own -- whatever it already
    // ships for its own UI will do. The data is copied.
    //
    // Calling it is optional. Without it, JSFX text is drawn with the small bitmap font
    // built into LICE: legible, fixed-width, ASCII only.
    //
    // Call it before opening any editor; it does not affect editors already open.
    void setJsfxEditorFontData(const void* data, size_t size);

    // Whether this build draws JSFX editor text itself rather than using the platform's
    // fonts, i.e. whether setJsfxEditorFontData() has any effect.
    bool jsfxUsesOwnFontBackend();

    // JSFX, the effects language REAPER ships, hosted by ysfx.
    //
    // This is an application-provided plugin format: construct one, hand it to
    // AudioPluginHostingAPI::addPluginFormat() before the first scan, and keep it alive for
    // as long as the host is used. Its effects then appear in the plugin catalog and
    // instantiate like any other format's.
    //
    // Nothing in uapmd-plugin-hosting knows about JSFX, and nothing here exposes ysfx, so
    // an application that links this module does not inherit ysfx's headers.
    class JsfxPluginFormat : public uapmd_plugin_hosting::AudioPluginFormat {
        struct Impl;
        std::unique_ptr<Impl> impl_;

    public:
        JsfxPluginFormat();
        ~JsfxPluginFormat() override;

        std::string name() override;

        // JSFX has no main thread affinity. Effects are compiled and run wherever they are
        // asked to be, which is unusual among plugin formats and worth taking advantage of.
        uapmd_plugin_hosting::AudioPluginUIThreadRequirement requiresUIThreadOn(
                uapmd_plugin_hosting::AudioPluginCatalogEntry* entry) override;

        uapmd_plugin_hosting::AudioPluginScanning* scanning() override;

        void createInstance(
                uapmd_plugin_hosting::AudioPluginCatalogEntry* entry,
                const uapmd_plugin_hosting::AudioPluginInstantiationOptions& options,
                std::function<void(std::unique_ptr<uapmd_plugin_hosting::AudioPluginInstanceAPI> instance,
                                   std::string error)> callback) override;

        // The locations searched in addition to, or instead of, the platform's
        // conventional ones. Set these before scanning; changing them afterwards requires a
        // rescan, because removing a location has to remove its effects from the catalog.
        void searchPaths(std::vector<std::string> paths, bool alsoUseDefaults);
        std::vector<std::string> searchPaths() const;
        bool useDefaultSearchPaths() const;

        // Where JSFX effects conventionally live on this platform. It is empty on Android,
        // iOS and the web, which have no REAPER installation to borrow from; there the
        // application supplies a location of its own through searchPaths().
        std::vector<std::filesystem::path> defaultSearchPaths() const;
    };

}
