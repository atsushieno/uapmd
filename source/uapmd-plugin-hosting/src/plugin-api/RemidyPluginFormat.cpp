#include "RemidyPluginFormat.hpp"
#include "RemidyAudioPluginInstance.hpp"
#include "RemidyTypeConversion.hpp"

namespace uapmd_plugin_hosting {

    namespace {
        AudioPluginScanning::ScanningStrategy toScanningStrategy(remidy::PluginScanning::ScanningStrategyValue value) {
            switch (value) {
                case remidy::PluginScanning::ScanningStrategyValue::NEVER:
                    return AudioPluginScanning::ScanningStrategy::Never;
                case remidy::PluginScanning::ScanningStrategyValue::MAYBE:
                    return AudioPluginScanning::ScanningStrategy::Maybe;
                case remidy::PluginScanning::ScanningStrategyValue::ALWAYS:
                    return AudioPluginScanning::ScanningStrategy::Always;
            }
            return AudioPluginScanning::ScanningStrategy::Never;
        }
    }

    namespace remidy_scanning {
        bool scanningMayBeSlow(remidy::PluginScanning* scanning) {
            return scanning && scanning->scanningMayBeSlow();
        }

        AudioPluginScanning::ScanningStrategy scanRequiresLoadLibrary(remidy::PluginScanning* scanning) {
            if (!scanning)
                return AudioPluginScanning::ScanningStrategy::Never;
            return toScanningStrategy(scanning->scanRequiresLoadLibrary());
        }

        bool scanRequiresLoadLibrary(remidy::PluginScanning* scanning, const std::filesystem::path& bundlePath) {
            return scanning && scanning->scanRequiresLoadLibrary(bundlePath);
        }

        AudioPluginScanning::ScanningStrategy scanRequiresInstantiation(remidy::PluginScanning* scanning) {
            if (!scanning)
                return AudioPluginScanning::ScanningStrategy::Never;
            return toScanningStrategy(scanning->scanRequiresInstantiation());
        }

        std::vector<AudioPluginCatalogEntry> getAllFastScannablePlugins(remidy::PluginScanning* scanning) {
            std::vector<AudioPluginCatalogEntry> ret{};
            if (!scanning)
                return ret;
            for (auto& e : scanning->getAllFastScannablePlugins())
                ret.emplace_back(toAudioPluginCatalogEntry(e));
            return ret;
        }

        void startSlowPluginScan(remidy::PluginScanning* scanning,
                                 AudioPluginFoundCallback pluginFound,
                                 AudioPluginScanCompletedCallback scanCompleted) {
            if (!scanning) {
                if (scanCompleted)
                    scanCompleted("Plugin format has no scanning support.");
                return;
            }
            scanning->startSlowPluginScan(
                [pluginFound = std::move(pluginFound)](remidy::PluginCatalogEntry entry) {
                    if (pluginFound)
                        pluginFound(toAudioPluginCatalogEntry(entry));
                },
                std::move(scanCompleted));
        }
    }

    bool RemidyPluginFileOrUrlScanning::usePluginSearchPaths() {
        return scanning_->usePluginSearchPaths();
    }

    std::vector<std::filesystem::path>& RemidyPluginFileOrUrlScanning::getDefaultSearchPaths() {
        return scanning_->getDefaultSearchPaths();
    }

    std::vector<std::filesystem::path> RemidyPluginFileOrUrlScanning::enumerateCandidateBundles(bool requireFastScanning) {
        return scanning_->enumerateCandidateBundles(requireFastScanning);
    }

    void RemidyPluginFileOrUrlScanning::scanBundle(const std::filesystem::path& bundlePath,
                                                   bool requireFastScanning,
                                                   double timeoutSeconds,
                                                   AudioPluginFoundCallback pluginFound,
                                                   AudioPluginScanCompletedCallback scanCompleted) {
        scanning_->scanBundle(bundlePath, requireFastScanning, timeoutSeconds,
            [pluginFound = std::move(pluginFound)](remidy::PluginCatalogEntry entry) {
                if (pluginFound)
                    pluginFound(toAudioPluginCatalogEntry(entry));
            },
            std::move(scanCompleted));
    }

    RemidyPluginFormat::RemidyPluginFormat(remidy::PluginFormat* format) : format_(format) {
        auto* scanning = format_ ? format_->scanning() : nullptr;
        if (auto* fileScanning = dynamic_cast<remidy::FileOrUrlBasedPluginScanning*>(scanning))
            scanning_ = std::make_unique<RemidyPluginFileOrUrlScanning>(fileScanning);
        else if (scanning)
            scanning_ = std::make_unique<RemidyPluginScanning>(scanning);
    }

    std::string RemidyPluginFormat::name() {
        return format_ ? format_->name() : std::string{};
    }

    AudioPluginScanning* RemidyPluginFormat::scanning() {
        return scanning_.get();
    }

    remidy::PluginCatalogEntry* RemidyPluginFormat::ensureRemidyEntry(AudioPluginCatalogEntry* entry) {
        if (!entry)
            return nullptr;
        auto it = remidy_entries_.find(entry->pluginId());
        if (it != remidy_entries_.end())
            return it->second.get();
        auto converted = std::make_unique<remidy::PluginCatalogEntry>(toRemidyCatalogEntry(*entry));
        auto* ret = converted.get();
        remidy_entries_[entry->pluginId()] = std::move(converted);
        return ret;
    }

    AudioPluginUIThreadRequirement RemidyPluginFormat::requiresUIThreadOn(AudioPluginCatalogEntry* entry) {
        if (!format_)
            return UIThreadNotRequired;
        return toAudioPluginUIThreadRequirement(format_->requiresUIThreadOn(ensureRemidyEntry(entry)));
    }

    void RemidyPluginFormat::createInstance(AudioPluginCatalogEntry* entry,
                                            const AudioPluginInstantiationOptions& options,
                                            std::function<void(std::unique_ptr<AudioPluginInstanceAPI> instance, std::string error)> callback) {
        if (!format_) {
            callback(nullptr, "Plugin format is not available.");
            return;
        }
        auto* remidyEntry = ensureRemidyEntry(entry);
        if (!remidyEntry) {
            callback(nullptr, "Plugin not found.");
            return;
        }
        format_->createInstance(
            remidyEntry,
            remidy::PluginFormat::PluginInstantiationOptions{toRemidyUIThreadRequirement(options.uiThreadRequirement)},
            [options, callback = std::move(callback)](std::unique_ptr<remidy::PluginInstance> instance, std::string error) mutable {
                if (!error.empty()) {
                    callback(nullptr, error);
                    return;
                }
                if (!instance) {
                    callback(nullptr, "Plugin format reported no error but created no instance.");
                    return;
                }
                auto api = std::make_unique<RemidyAudioPluginInstance>(std::move(instance));
                remidy::PluginInstance::ConfigurationRequest configuration{};
                configuration.sampleRate = options.sampleRate;
                configuration.bufferSizeInSamples = options.bufferSizeInSamples;
                configuration.offlineMode = options.offlineMode;
                configuration.mainInputChannels = options.mainInputChannels;
                configuration.mainOutputChannels = options.mainOutputChannels;
                auto code = api->configure(configuration);
                if (code != remidy::StatusCode::OK) {
                    callback(nullptr, std::format("configure() failed. Error code {}", static_cast<int32_t>(code)));
                    return;
                }
                callback(std::move(api), "");
            });
    }

    AudioPluginUIThreadRequirement toAudioPluginUIThreadRequirement(remidy::PluginUIThreadRequirement source) {
        return static_cast<AudioPluginUIThreadRequirement>(static_cast<uint32_t>(source));
    }

    remidy::PluginUIThreadRequirement toRemidyUIThreadRequirement(AudioPluginUIThreadRequirement source) {
        return static_cast<remidy::PluginUIThreadRequirement>(static_cast<uint32_t>(source));
    }

}
