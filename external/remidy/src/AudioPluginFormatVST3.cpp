
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
        FUnknown* instance;
    public:
        explicit AudioPluginInstanceVST3(FUnknown* instance) : instance(instance) {
        }

        ~AudioPluginInstanceVST3() override {
            // FIXME: release instance
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
        auto library = loadLibraryFromBundle(vst3Id->info.bundlePath);
        if (!library)
            return nullptr;
        const auto factory = getFactoryFromLibrary(library);
        if (!factory)
            return nullptr;
        FUnknown* instance{};
        v3_factory_info factoryInfo{};
        auto result = factory->vtable->factory.get_factory_info(factory, &factoryInfo);
        if (result != 0)
            // FIXME: report error
            return nullptr;
        for (int classIdx = 0, n = factory->vtable->factory.num_classes(factory); classIdx < n; classIdx++) {
            v3_class_info classInfo{};
            factory->vtable->factory.get_class_info(factory, classIdx, &classInfo);
            if (!memcmp(classInfo.class_id, v3_component_iid, sizeof(v3_tuid)))
                continue;
            result = factory->vtable->factory.create_instance(factory, vst3Id->info.tuid, v3_funknown_iid, (void**) &instance);
            if (result) {
                std::cerr << "Failed to create FUnknown for " << uniqueId->getDisplayName() << " result: " << result << std::endl;
                return nullptr;
            }
            IComponent *component{};
            result = instance->vtable->unknown.query_interface(instance, v3_component_iid, (void**) &component);
            if (result) {
                std::cerr << "Failed to create VST3 instance: " << uniqueId->getDisplayName() << " result: " << result << std::endl;
                return nullptr;
            }

            HostApplication host{};
            result = component->vtable->base.initialize(instance, (v3_funknown**) &host);
            if (result) {
                std::cerr << "Failed to initialize vst3: " << uniqueId->getDisplayName() << std::endl;
                return nullptr;
            }

            return new AudioPluginInstanceVST3(instance);
        }
        return nullptr;
    }
}
