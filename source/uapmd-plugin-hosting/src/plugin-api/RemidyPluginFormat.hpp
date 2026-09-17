#pragma once

#include <map>
#include <memory>
#include <string>

#include "remidy/remidy.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

// The AudioPluginFormat implementation for the plugin formats that remidy provides
// (VST3, AU, LV2, CLAP, AAP, WebCLAP). It is private to the module: everything above it
// sees only AudioPluginFormat, so an application-provided format is indistinguishable
// from a built-in one.

namespace uapmd_plugin_hosting {

    class RemidyPluginFormat;

    // The members that every remidy-backed scanning adapter shares, regardless of whether
    // the format is file/URL based. They are free functions because the two adapters below
    // derive from different interfaces and so cannot share a common concrete base.
    namespace remidy_scanning {
        bool scanningMayBeSlow(remidy::PluginScanning* scanning);
        AudioPluginScanning::ScanningStrategy scanRequiresLoadLibrary(remidy::PluginScanning* scanning);
        bool scanRequiresLoadLibrary(remidy::PluginScanning* scanning, const std::filesystem::path& bundlePath);
        AudioPluginScanning::ScanningStrategy scanRequiresInstantiation(remidy::PluginScanning* scanning);
        std::vector<AudioPluginCatalogEntry> getAllFastScannablePlugins(remidy::PluginScanning* scanning);
        void startSlowPluginScan(remidy::PluginScanning* scanning,
                                 AudioPluginFoundCallback pluginFound,
                                 AudioPluginScanCompletedCallback scanCompleted);
    }

    // Wraps a remidy::PluginScanning that is not file/URL based (AU).
    class RemidyPluginScanning : public AudioPluginScanning {
        remidy::PluginScanning* scanning_;

    public:
        explicit RemidyPluginScanning(remidy::PluginScanning* scanning) : scanning_(scanning) {}

        bool scanningMayBeSlow() override { return remidy_scanning::scanningMayBeSlow(scanning_); }
        ScanningStrategy scanRequiresLoadLibrary() override { return remidy_scanning::scanRequiresLoadLibrary(scanning_); }
        bool scanRequiresLoadLibrary(const std::filesystem::path& bundlePath) override { return remidy_scanning::scanRequiresLoadLibrary(scanning_, bundlePath); }
        ScanningStrategy scanRequiresInstantiation() override { return remidy_scanning::scanRequiresInstantiation(scanning_); }
        std::vector<AudioPluginCatalogEntry> getAllFastScannablePlugins() override { return remidy_scanning::getAllFastScannablePlugins(scanning_); }
        void startSlowPluginScan(AudioPluginFoundCallback pluginFound, AudioPluginScanCompletedCallback scanCompleted) override {
            remidy_scanning::startSlowPluginScan(scanning_, std::move(pluginFound), std::move(scanCompleted));
        }
    };

    // Wraps a remidy::FileOrUrlBasedPluginScanning (VST3, LV2, CLAP, AAP, WebCLAP).
    // Which of the two adapters a format gets decides whether the scanner is allowed to
    // scan it bundle by bundle, so the distinction has to survive the wrapping.
    class RemidyPluginFileOrUrlScanning : public AudioPluginFileOrUrlScanning {
        remidy::FileOrUrlBasedPluginScanning* scanning_;

    public:
        explicit RemidyPluginFileOrUrlScanning(remidy::FileOrUrlBasedPluginScanning* scanning) : scanning_(scanning) {}

        bool scanningMayBeSlow() override { return remidy_scanning::scanningMayBeSlow(scanning_); }
        ScanningStrategy scanRequiresLoadLibrary() override { return remidy_scanning::scanRequiresLoadLibrary(scanning_); }
        bool scanRequiresLoadLibrary(const std::filesystem::path& bundlePath) override { return remidy_scanning::scanRequiresLoadLibrary(scanning_, bundlePath); }
        ScanningStrategy scanRequiresInstantiation() override { return remidy_scanning::scanRequiresInstantiation(scanning_); }
        std::vector<AudioPluginCatalogEntry> getAllFastScannablePlugins() override { return remidy_scanning::getAllFastScannablePlugins(scanning_); }
        void startSlowPluginScan(AudioPluginFoundCallback pluginFound, AudioPluginScanCompletedCallback scanCompleted) override {
            remidy_scanning::startSlowPluginScan(scanning_, std::move(pluginFound), std::move(scanCompleted));
        }

        bool usePluginSearchPaths() override;
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        std::vector<std::filesystem::path> enumerateCandidateBundles(bool requireFastScanning) override;
        void scanBundle(const std::filesystem::path& bundlePath,
                        bool requireFastScanning,
                        double timeoutSeconds,
                        AudioPluginFoundCallback pluginFound,
                        AudioPluginScanCompletedCallback scanCompleted) override;
    };

    class RemidyPluginFormat : public AudioPluginFormat {
        remidy::PluginFormat* format_;
        std::unique_ptr<AudioPluginScanning> scanning_{};
        // remidy::PluginInstance holds on to the PluginCatalogEntry it was created from and
        // hands it back through info(), so the remidy-side entries have to outlive their
        // instances. They are kept here, keyed by plugin ID, and created on demand: an entry
        // can reach us from a scan we ran, or from the plugin list cache that we never saw.
        std::map<std::string, std::unique_ptr<remidy::PluginCatalogEntry>> remidy_entries_{};

        remidy::PluginCatalogEntry* ensureRemidyEntry(AudioPluginCatalogEntry* entry);

    public:
        explicit RemidyPluginFormat(remidy::PluginFormat* format);

        remidy::PluginFormat* rawFormat() const { return format_; }

        std::string name() override;
        AudioPluginUIThreadRequirement requiresUIThreadOn(AudioPluginCatalogEntry* entry) override;
        AudioPluginScanning* scanning() override;
        void createInstance(AudioPluginCatalogEntry* entry,
                            const AudioPluginInstantiationOptions& options,
                            std::function<void(std::unique_ptr<AudioPluginInstanceAPI> instance, std::string error)> callback) override;
    };

}
