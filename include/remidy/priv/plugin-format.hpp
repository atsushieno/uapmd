#pragma once

#include <future>
#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormat {
        class Impl;
        Impl *impl{};

    protected:
        AudioPluginFormat();
        virtual ~AudioPluginFormat() = default;

    public:
        virtual std::string name() = 0;

        // Provides format-specific extension points.
        // Cast to the type of the format to access those format-specifics.
        virtual AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() { return nullptr; }

        // Indicates whether you should invoke plugin methods on the UI/main thread.
        //
        // VST3 and CLAP requires almost all plugin operations performed on the main thread, while
        // AudioUnit and LV2 don't. This impacts the overall app performance (take it like you have
        // a GIL in Python, or forced single-threaded COM model). Also, in those free-threaded model
        // you have to pay attention to accesses to the plugin resources (you should).
        //
        // This method is marked as virtual so that you can actually override this and opt in to
        // process certain VST/CLAP plugins work in better "multi thread ready" way, or on the other
        // hand treat some AU/LV2 plugins work only on the main thread when any of plugins in those
        // formats behave unstable.
        virtual AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) = 0;

        // Returns true if scanRequiresLoadLibrary() does not return `ScanningStrategyValue::NEVER`.
        // Usually this means that you should resort to plugin list cache.
        bool scanningMayBeSlow();

        // Indicates whether the plugin API requires sample rate at *instantiating*.
        // Only LV2 requires this, among VST3, AUv2/v3, LV2, and CLAP.
        // This impacts on whether we will have to discard and instantiate a plugin
        // when our use app changes the sample rate.
        bool instantiateRequiresSampleRate();

        enum class ScanningStrategyValue {
            NEVER,
            MAYBE,
            ALWAYS
        };
        // Indicates whether scanning of the plugin requires loading of the library.
        // `ALWAYS` for CLAP, `MAYBE` for VST3 (when `moduleinfo.json` does not exist), `NONE` for AU and LV2.
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        // Indicates whether scanning of the plugin requires instancing of the plugin IF it needs to load the library.
        // NEVER for AU, LV2, and CLAP. ALWAYS for VST3.
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        // Implements plugin scanning. You might want to use this functionality in a separate process as
        // some bad behaving plugins may crash the process.
        virtual std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() = 0;

        struct InvokeResult {
            std::unique_ptr<AudioPluginInstance> instance;
            std::string error;
        };

        // Asynchronously creates a plugin instance.
        virtual void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) = 0;
    };

    // Desktop specific plugin format members.
    // FIXME: maybe this should become extracted as a file system adapter rather than a base class
    // as it brings in extraneous inheritance layer. It is not elegant.
    class FileBasedAudioPluginFormat : public AudioPluginFormat {
    protected:
        explicit FileBasedAudioPluginFormat() = default;

        // Indicates that scanning of the plugins in this format is based on file paths (VST3, LV2, CLAP vs. AU).
        virtual bool usePluginSearchPaths() = 0;
        // Provides the default search paths for the format, if its plugin scanning is file-based.
        virtual std::vector<std::filesystem::path>& getDefaultSearchPaths() = 0;

        std::vector<std::string> overrideSearchPaths{};

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }
    };
}