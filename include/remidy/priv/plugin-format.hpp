#pragma once

#include <future>
#include "../remidy.hpp"

namespace remidy {
    class PluginFormat {
        class Impl;
        Impl *impl{};

    protected:
        PluginFormat();
        virtual ~PluginFormat() = default;

    public:
        virtual std::string name() = 0;

        // Provides format-specific extension points.
        // Cast to the type of the format to access those format-specifics.
        virtual PluginExtensibility<PluginFormat>* getExtensibility() { return nullptr; }

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
        virtual PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) = 0;

        virtual PluginScanner* scanner() = 0;

        // Indicates whether the plugin API requires sample rate at *instantiating*.
        // Only LV2 requires this, among VST3, AUv2/v3, LV2, and CLAP.
        // This impacts on whether we will have to discard and instantiate a plugin
        // when our use app changes the sample rate.
        bool instantiateRequiresSampleRate();

        struct InvokeResult {
            std::unique_ptr<PluginInstance> instance;
            std::string error;
        };

        // Asynchronously creates a plugin instance.
        virtual void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) = 0;
    };
}
