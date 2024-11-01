#include <atomic>

#include "remidy.hpp"
#include <lilv/lilv.h>

#include "lv2/LV2Helper.hpp"

namespace remidy {
    class AudioPluginScannerLV2 : public FileBasedPluginScanner {
        LilvWorld* world;
    public:
        AudioPluginScannerLV2(LilvWorld* world) : world(world) {}

        bool usePluginSearchPaths() override { return true; }
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatLV2::Impl {
        PluginFormatLV2* owner;
        Logger* logger;
        Extensibility extensibility;
        AudioPluginScannerLV2 lv2_scanner{nullptr};

    public:
        explicit Impl(PluginFormatLV2* owner);
        ~Impl();

        auto format() const { return owner; }
        LilvWorld *world;
        remidy_lv2::LV2ImplWorldContext *worldContext;
        std::vector<LV2_Feature*> features{};

        PluginExtensibility<PluginFormat>* getExtensibility();
        PluginScanner* scanner() { return &lv2_scanner; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback);
        void unrefLibrary(PluginCatalogEntry& info);
        PluginCatalog createCatalogFragment(const std::filesystem::path &bundlePath);
    };

    class AudioPluginInstanceLV2 : public PluginInstance {
        PluginCatalogEntry* entry;
        PluginFormatLV2::Impl* formatImpl;
        const LilvPlugin* plugin;
        LilvInstance* instance{nullptr};
        remidy_lv2::LV2ImplPluginContext implContext;

        struct BusSearchResult {
            uint32_t numAudioIn{0};
            uint32_t numAudioOut{0};
            uint32_t numEventIn{0};
            uint32_t numEventOut{0};
        };
        BusSearchResult buses;
        BusSearchResult inspectBuses();
        std::vector<AudioBusConfiguration*> input_buses;
        std::vector<AudioBusConfiguration*> output_buses;

    public:
        explicit AudioPluginInstanceLV2(PluginCatalogEntry* entry, PluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin);
        ~AudioPluginInstanceLV2() override;

        PluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return formatImpl->format()->requiresUIThreadOn(entry);
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;
        StatusCode process(AudioProcessContext &process) override;

        // port helpers
        bool hasAudioInputs() override { return buses.numAudioIn > 0; }
        bool hasAudioOutputs() override { return buses.numAudioOut > 0; }
        bool hasEventInputs() override { return buses.numEventIn > 0; }
        bool hasEventOutputs() override { return buses.numEventOut > 0; }

        const std::vector<AudioBusConfiguration*> audioInputBuses() const override;
        const std::vector<AudioBusConfiguration*> audioOutputBuses() const override;
    };

    PluginFormatLV2::Impl::Impl(PluginFormatLV2* owner) :
        owner(owner),
        logger(Logger::global()),
        extensibility(*owner) {
        world = lilv_world_new();
        lv2_scanner = AudioPluginScannerLV2(world);
        // FIXME: setup paths
        lilv_world_load_all(world);

        // This also initializes features
        worldContext = new remidy_lv2::LV2ImplWorldContext(logger, world);
    }
    PluginFormatLV2::Impl::~Impl() {
        delete worldContext;
        lilv_free(world);
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginScannerLV2::scanAllAvailablePlugins() {
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};

        auto plugins = lilv_world_get_all_plugins(world);
        LILV_FOREACH(plugins, iter, plugins) {
            const LilvPlugin* plugin = lilv_plugins_get(plugins, iter);
            auto entry = std::make_unique<PluginCatalogEntry>();
            static std::string lv2Format{"LV2"};
            entry->format(lv2Format);
            auto uriNode = lilv_plugin_get_uri(plugin);
            std::string uri = lilv_node_as_uri(uriNode);
            auto bundleUriNode = lilv_plugin_get_bundle_uri(plugin);
            auto bundlePath = lilv_node_as_uri(bundleUriNode);
            entry->bundlePath(std::filesystem::path{bundlePath});
            entry->pluginId(uri);
            auto nameNode = lilv_plugin_get_name(plugin);
            std::string name = lilv_node_as_string(nameNode);
            auto authorNameNode = lilv_plugin_get_author_name(plugin);
            auto authorName = lilv_node_as_string(authorNameNode);
            auto authorUrlNode = lilv_plugin_get_author_homepage(plugin);
            auto authorUrl = lilv_node_as_string(authorUrlNode);
            entry->displayName(name);
            entry->vendorName(authorName);
            entry->productUrl(authorUrl);
            ret.emplace_back(std::move(entry));
        }
        return ret;
    }

    void PluginFormatLV2::Impl::createInstance(
        PluginCatalogEntry* info,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
    ) {
        auto targetUri = lilv_new_uri(world, info->pluginId().c_str());
        auto plugins = lilv_world_get_all_plugins(world);
        auto plugin = lilv_plugins_get_by_uri(plugins, targetUri);
        if (plugin) {
            auto instance = std::make_unique<AudioPluginInstanceLV2>(info, this, plugin);
            callback(std::move(instance), "");
            return;
        }
        callback(nullptr, std::string{"Plugin '"} + info->pluginId() + "' was not found");
    }

    void PluginFormatLV2::Impl::unrefLibrary(PluginCatalogEntry& info) {
    }

    PluginCatalog PluginFormatLV2::Impl::createCatalogFragment(const std::filesystem::path &bundlePath) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::createCatalogFragment() is not implemented");
    }

    PluginExtensibility<PluginFormat> * PluginFormatLV2::Impl::getExtensibility() {
        return &extensibility;
    }

    PluginFormatLV2::PluginFormatLV2(std::vector<std::string> &overrideSearchPaths) {
        impl = new Impl(this);
    }

    PluginFormatLV2::~PluginFormatLV2() {
        delete impl;
    }

    PluginExtensibility<PluginFormat> * PluginFormatLV2::getExtensibility() {
        return impl->getExtensibility();
    }

    PluginScanner * PluginFormatLV2::scanner() {
        return impl->scanner();
    }

    std::vector<std::filesystem::path>& AudioPluginScannerLV2::getDefaultSearchPaths() {
        static std::filesystem::path defaultSearchPathsLV2[] = {
#if _WIN32
            std::string(getenv("APPDATA")) + "\\LV2",
            std::string(getenv("COMMONPROGRAMFILES")) + "\\LV2"
#elif __APPLE__
            std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/LV2",
            "/Library/Audio/Plug-Ins/LV2"
#else // We assume the rest covers Linux and other Unix-y platforms
            std::string(getenv("HOME")) + "/.lv2",
            "/usr/local/lib/lv2", // $PREFIX-based path
            "/usr/lib/lv2" // $PREFIX-based path
#endif
        };
        static std::vector<std::filesystem::path> ret = [] {
            std::vector<std::filesystem::path> paths{};
            for (auto& path : defaultSearchPathsLV2)
                paths.emplace_back(path);
            return paths;
        }();
        return ret;
    }

    void PluginFormatLV2::createInstance(PluginCatalogEntry* info,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)>&& callback) {
        impl->createInstance(info, callback);
    }

    PluginFormatLV2::Extensibility::Extensibility(PluginFormat &format) :
        PluginExtensibility(format) {
    }

    // AudioPluginInstanceLV2

    AudioPluginInstanceLV2::AudioPluginInstanceLV2(PluginCatalogEntry* entry, PluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin) :
        entry(entry), formatImpl(formatImpl), plugin(plugin),
        implContext(formatImpl->worldContext, formatImpl->world, plugin) {
        buses = inspectBuses();
    }

    AudioPluginInstanceLV2::~AudioPluginInstanceLV2() {
        if (instance)
            lilv_instance_free(instance);
        instance = nullptr;
    }

    StatusCode AudioPluginInstanceLV2::configure(ConfigurationRequest& configuration) {
        // Do we have to deal with offlineMode? LV2 only mentions hardRT*Capable*.

        if (instance)
            // we need to save state delete instance, recreate instance with the
                // new configuration, and restore the state.
                    throw std::runtime_error("AudioPluginInstanceLV2::configure() re-configuration is not implemented");

        instance = remidy_lv2::instantiate_plugin(formatImpl->worldContext, &implContext, plugin,
            configuration.sampleRate, configuration.offlineMode);
        if (!instance)
            return StatusCode::FAILED_TO_INSTANTIATE;

        return StatusCode::OK;
    }

    StatusCode AudioPluginInstanceLV2::startProcessing() {
        if (!instance)
            return StatusCode::ALREADY_INVALID_STATE;
        lilv_instance_activate(instance);
        return StatusCode::OK;
    }

    StatusCode AudioPluginInstanceLV2::stopProcessing() {
        if (!instance)
            return StatusCode::ALREADY_INVALID_STATE;
        lilv_instance_deactivate(instance);
        return StatusCode::OK;
    }

    StatusCode AudioPluginInstanceLV2::process(AudioProcessContext &process) {
        // FIXME: implement
        std::cerr << "AudioPluginInstanceLV2::process() is not implemented" << std::endl;
        return StatusCode::FAILED_TO_PROCESS;
    }

    AudioPluginInstanceLV2::BusSearchResult AudioPluginInstanceLV2::inspectBuses() {
        BusSearchResult ret{};

        // FIXME: we need to fill input_buses and output_buses here.
        for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
            auto port = lilv_plugin_get_port_by_index(plugin, p);
            if (implContext.IS_AUDIO_IN(plugin, port))
                ret.numAudioIn++;
            else if (implContext.IS_AUDIO_OUT(plugin, port))
                ret.numAudioOut++;
            if (implContext.IS_ATOM_IN(plugin, port))
                ret.numEventIn++;
            if (implContext.IS_ATOM_OUT(plugin, port))
                ret.numEventOut++;
        }
        return ret;
    }

    const std::vector<AudioBusConfiguration*> AudioPluginInstanceLV2::audioInputBuses() const { return input_buses; }
    const std::vector<AudioBusConfiguration*> AudioPluginInstanceLV2::audioOutputBuses() const { return output_buses; }
}
