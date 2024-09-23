
#include <cassert>
#include <dlfcn.h>
#include <iostream>
#include "VST3Helper.hpp"
#include "AudioPluginFormatVST3.hpp"

namespace remidy {

    class AudioPluginIdentifierVST3 : public AudioPluginIdentifier {
    private:
        std::string idString{};
    public:
        std::string & getVendor() override;

        std::string & getUrl() override;

        std::string& getUniqueId() override;

        std::string& getDisplayName() override;

    private:
    public:
        PluginClassInfo info;

        explicit AudioPluginIdentifierVST3(PluginClassInfo& info) : info(info) {
            idString = reinterpret_cast<char *>(info.tuid);
        }
    };

    std::string & AudioPluginIdentifierVST3::getVendor() { return info.vendor; }
    std::string & AudioPluginIdentifierVST3::getUrl() { return info.url; }
    std::string& AudioPluginIdentifierVST3::getUniqueId() { return idString; }
    std::string & AudioPluginIdentifierVST3::getDisplayName() { return info.className; }

    class AudioPluginFormatVST3::Impl {
        AudioPluginFormatVST3* owner;

    public:
        explicit Impl(AudioPluginFormatVST3* owner) : owner(owner) {}

        std::vector<std::unique_ptr<AudioPluginIdentifierVST3>> plugin_list_cache{};
        void scanAllAvailablePlugins();
    };

    std::vector<std::string>& AudioPluginFormatVST3::getDefaultSearchPaths() {
        static std::string defaultSearchPathsVST3[] = {
#if _WIN32
            std::string(getenv("LOCALAPPDATA")) + "\\Programs\\Common\\VST3",
            std::string(getenv("PROGRAMFILES")) + "\\Common Files\\VST3",
            std::string(getenv("PROGRAMFILES(x86)")) + "\\Common Files\\VST3"
#elif __APPLE__
            std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/VST3",
            "/Library/Audio/Plug-Ins/VST3",
            "/Network/Library/Audio/Plug-Ins/VST3"
#else // We assume the rest covers Linux and other Unix-y platforms
            std::string(getenv("HOME")) + "/.vst3",
            "/usr/lib/vst3",
            "/usr/local/lib/vst3"
#endif
        };
        static std::vector<std::string> ret = [] {
            std::vector<std::string> paths{};
            paths.append_range(defaultSearchPathsVST3);
            return paths;
        }();
        return ret;
    }

    AudioPluginFormatVST3::AudioPluginFormatVST3(std::vector<std::string> &overrideSearchPaths)
        : DesktopAudioPluginFormat() {
        impl = new Impl(this);
    }
    AudioPluginFormatVST3::~AudioPluginFormatVST3() {
        delete impl;
    }


    bool AudioPluginFormatVST3::usePluginSearchPaths() { return true;}

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresLoadLibrary() { return YES; }

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresInstantiation() { return MAYBE; }

    std::vector<AudioPluginIdentifier*> AudioPluginFormatVST3::scanAllAvailablePlugins() {
        std::vector<AudioPluginIdentifier*> ret{};
        impl->scanAllAvailablePlugins();
        for (auto& id : impl->plugin_list_cache)
            ret.emplace_back(id.get());
        return ret;
    }

    void AudioPluginFormatVST3::Impl::scanAllAvailablePlugins() {
        std::vector<PluginClassInfo> infos;
        for (auto &path : owner->getDefaultSearchPaths()) {
            std::filesystem::path dir{path};
            if (is_directory(dir)) {
                for (auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (!strcasecmp(entry.path().extension().c_str(), ".vst3")) {
                        scanAllAvailablePluginsFromLibrary(entry.path(), infos);
                    }
                }
            }
        }
        for (auto &id : plugin_list_cache)
            id.reset();
        plugin_list_cache.clear();
        for (auto &info : infos)
            plugin_list_cache.emplace_back(std::make_unique<AudioPluginIdentifierVST3>(info));
    }

    class AudioPluginInstanceVST3 : public AudioPluginInstance {
    public:
        remidy_status_t configure(int32_t sampleRate) override;

        remidy_status_t process(AudioProcessContext &process) override;

    private:
        IComponent* component;
        FUnknown* instance;
    public:
        explicit AudioPluginInstanceVST3(
            IComponent* component,
            FUnknown* instance
        ) : component(component), instance(instance) {
        }

        ~AudioPluginInstanceVST3() override {
            // FIXME: release instance
            //component->vtable->base.terminate(instance);
            //instance->vtable->unknown.unref(instance);
        }
    };

    remidy_status_t AudioPluginInstanceVST3::configure(int32_t sampleRate) {
        throw std::runtime_error("AudioPluginInstanceVST3::configure() not implemented");
    }

    remidy_status_t AudioPluginInstanceVST3::process(AudioProcessContext &process) {
        throw std::runtime_error("AudioPluginInstanceVST3::process() not implemented");
    }

    AudioPluginInstance* AudioPluginFormatVST3::createInstance(AudioPluginIdentifier *uniqueId) {
        auto vst3Id = (AudioPluginIdentifierVST3*) uniqueId;
        AudioPluginInstanceVST3* ret{nullptr};
        HostApplication host{};

        forEachPlugin(vst3Id->info.bundlePath, [&](IPluginFactory* factory, PluginClassInfo &info) {

            IPluginFactory3* factory3{nullptr};
            auto result = factory->vtable->unknown.query_interface(factory, v3_plugin_factory_3_iid, (void**) &factory3);
            if (result == V3_OK) {
                result = factory3->vtable->factory_3.set_host_context(factory3, (v3_funknown**) &host);
                // There are weird plugins that "implements IPluginFactory3" and then returns kNotImplemented :angry:
                //  in that case, it is not callable anyway, so treat it as if IPluginFactory3 were not queryable.
                factory3->vtable->unknown.unref(factory3);
                if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
                    std::cerr << "Failed to set HostApplication to IPluginFactory3: " << uniqueId->getDisplayName() << " result: " << result << std::endl;
                    return;
                }
            }

            /*
            FUnknown* compatibility{};
            result = factory->vtable->factory.create_instance(factory, vst3Id->info.tuid, v3_compatibility_iid, (void**) &compatibility);
            if (result) // not about this class
                return;*/

            FUnknown* instance{};
            result = factory->vtable->factory.create_instance(factory, vst3Id->info.tuid, v3_component_iid, (void**) &instance);
            if (result) // not about this class
                return;

            bool success = false;
            IComponent *component{};
            result = instance->vtable->unknown.query_interface(instance, v3_component_iid, (void**) &component);
            if (result == V3_OK) {
                try {
                    result = component->vtable->base.initialize(instance, (v3_funknown**) &host);
                    if (result == V3_OK) {
                        ret = new AudioPluginInstanceVST3(component, instance);
                        return;
                    }
                    std::cerr << "Failed to initialize vst3: " << uniqueId->getDisplayName() << std::endl;
                } catch (...) {
                    std::cerr << "Crash on initializing vst3: " << uniqueId->getDisplayName() << std::endl;
                }
            }
            else
                std::cerr << "Failed to query VST3 component: " << uniqueId->getDisplayName() << " result: " << result << std::endl;

            component->vtable->unknown.unref(component);
        });
        return ret;
    }
}
