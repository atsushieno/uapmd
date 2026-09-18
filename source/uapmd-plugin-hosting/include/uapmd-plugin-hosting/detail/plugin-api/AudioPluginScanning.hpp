#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "AudioPluginCatalog.hpp"

namespace uapmd_plugin_hosting {

    using AudioPluginScanCompletedCallback = std::function<void(std::string error)>;
    using AudioPluginFoundCallback = std::function<void(AudioPluginCatalogEntry entry)>;

    // How a format discovers the plugins that are installed.
    //
    // A format whose plugins can all be enumerated cheaply implements this interface alone
    // and reports scanningMayBeSlow() == false; everything it knows comes back from
    // getAllFastScannablePlugins().
    //
    // A format that has to load each plugin binary to learn about it is "slow", and must
    // implement AudioPluginFileOrUrlScanning instead so that the host can scan bundle by
    // bundle, cache the outcome, and isolate crashes into a separate scanner process.
    class AudioPluginScanning {
    public:
        virtual ~AudioPluginScanning() = default;

        enum class ScanningStrategy {
            Never,
            Maybe,
            Always
        };

        // Indicates that some plugins may be slow to scan, so that without a plugin list
        // cache the plugin list starts out incomplete.
        // A format that returns true must also implement AudioPluginFileOrUrlScanning.
        virtual bool scanningMayBeSlow() = 0;

        // Indicates whether scanning requires loading the plugin binary.
        virtual ScanningStrategy scanRequiresLoadLibrary() = 0;

        // Indicates whether scanning this particular bundle requires loading its binary.
        virtual bool scanRequiresLoadLibrary(const std::filesystem::path& bundlePath) = 0;

        // Indicates whether scanning requires instantiating the plugin, when it has to load
        // the binary at all.
        virtual ScanningStrategy scanRequiresInstantiation() = 0;

        // Returns every plugin that can be discovered without loading a plugin binary.
        // A partially fast scanner returns what it can here; slow scanning completes the list.
        virtual std::vector<AudioPluginCatalogEntry> getAllFastScannablePlugins() = 0;

        // Scans everything, including what is slow to discover. It reports each plugin
        // through `pluginFound` and finishes by calling `scanCompleted` exactly once.
        virtual void startSlowPluginScan(AudioPluginFoundCallback pluginFound,
                                         AudioPluginScanCompletedCallback scanCompleted) = 0;
    };

    // Scanning for formats whose plugins are bundles identified by a file path or a URL
    // (VST3, CLAP, LV2, WebCLAP), as opposed to being enumerated by the operating system (AU).
    //
    // Implementing this is what lets the host scan one bundle at a time: it can skip
    // bundles that are blocklisted, report progress, honor a per-bundle timeout, and run
    // the whole scan in a separate process so that a crashing plugin does not take the
    // application with it.
    class AudioPluginFileOrUrlScanning : public AudioPluginScanning {
        std::vector<std::string> override_search_paths_{};
        bool use_default_search_paths_{true};

    public:
        // Indicates that plugins are looked up in search paths, rather than at fixed locations.
        virtual bool usePluginSearchPaths() = 0;
        // The locations that are searched when no override search path is set.
        virtual std::vector<std::filesystem::path>& getDefaultSearchPaths() = 0;

        std::vector<std::string>& getOverrideSearchPaths() { return override_search_paths_; }
        void addSearchPath(const std::string& path) { override_search_paths_.emplace_back(path); }
        // Replaces the whole set, which is what an editable list of locations needs.
        // addSearchPath() only ever appends, and mutating the returned reference is not an
        // obvious thing for a caller to be allowed to do.
        void setOverrideSearchPaths(std::vector<std::string> paths) {
            override_search_paths_ = std::move(paths);
        }

        // Whether the platform's conventional locations are searched as well as the
        // override paths. Someone who keeps plugins somewhere else entirely turns this
        // off; the default locations are a separate list and cannot be edited away.
        //
        // A format decides for itself whether to honour this -- it only means anything to
        // one that consults getDefaultSearchPaths() in the first place.
        bool useDefaultSearchPaths() const { return use_default_search_paths_; }
        void useDefaultSearchPaths(bool value) { use_default_search_paths_ = value; }

        // Returns every bundle that is worth scanning. When `requireFastScanning` is set,
        // bundles that can only be scanned slowly may be left out.
        virtual std::vector<std::filesystem::path> enumerateCandidateBundles(bool requireFastScanning) = 0;

        // Scans the plugins contained in one bundle. It reports each plugin through
        // `pluginFound` and finishes by calling `scanCompleted` exactly once, with an empty
        // error on success. `timeoutSeconds` of 0 means no timeout.
        virtual void scanBundle(const std::filesystem::path& bundlePath,
                                bool requireFastScanning,
                                double timeoutSeconds,
                                AudioPluginFoundCallback pluginFound,
                                AudioPluginScanCompletedCallback scanCompleted) = 0;
    };

}
