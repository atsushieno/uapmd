#include <atomic>

#include "remidy.hpp"
#include <lilv/lilv.h>

#include "lv2/LV2Helper.hpp"

namespace remidy {
    class AudioPluginFormatLV2::Impl {
        AudioPluginFormatLV2* owner;
        Logger* logger;
        Extensibility extensibility;

    public:
        explicit Impl(AudioPluginFormatLV2* owner);
        ~Impl();

        LilvWorld *world;
        remidy_lv2::LV2ImplWorldContext *worldContext;
        std::vector<LV2_Feature*> features{};

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility();
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback);
        void unrefLibrary(PluginCatalogEntry *info);
        PluginCatalog createCatalogFragment(const std::filesystem::path &bundlePath);
    };

    class AudioPluginInstanceLV2 : public AudioPluginInstance {
        AudioPluginFormatLV2::Impl* formatImpl;
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

    public:
        explicit AudioPluginInstanceLV2(AudioPluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin);
        ~AudioPluginInstanceLV2() override;

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
    };

    AudioPluginFormatLV2::Impl::Impl(AudioPluginFormatLV2* owner) :
        owner(owner),
        logger(Logger::global()),
        extensibility(*owner) {
        world = lilv_world_new();
        // FIXME: setup paths
        lilv_world_load_all(world);

        // This also initializes features
        worldContext = new remidy_lv2::LV2ImplWorldContext(logger, world);
    }
    AudioPluginFormatLV2::Impl::~Impl() {
        delete worldContext;
        lilv_free(world);
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginFormatLV2::Impl::scanAllAvailablePlugins() {
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
            entry->setMetadataProperty(remidy::PluginCatalogEntry::DisplayName, name);
            entry->setMetadataProperty(remidy::PluginCatalogEntry::VendorName, authorName);
            entry->setMetadataProperty(remidy::PluginCatalogEntry::ProductUrl, authorUrl);
            ret.emplace_back(std::move(entry));
        }
        return ret;
    }

    void AudioPluginFormatLV2::Impl::createInstance(
        PluginCatalogEntry *info,
        std::function<void(InvokeResult)> callback
    ) {
        auto targetUri = lilv_new_uri(world, info->pluginId().c_str());
        auto plugins = lilv_world_get_all_plugins(world);
        auto plugin = lilv_plugins_get_by_uri(plugins, targetUri);
        if (plugin) {
            auto instance = std::make_unique<AudioPluginInstanceLV2>(this, plugin);
            callback(InvokeResult{std::move(instance), std::string{}});
            return;
        }
        callback(InvokeResult{nullptr, std::string{"Plugin '"} + info->pluginId() + "' was not found"});
    }

    void AudioPluginFormatLV2::Impl::unrefLibrary(PluginCatalogEntry *info) {
    }

    PluginCatalog AudioPluginFormatLV2::Impl::createCatalogFragment(const std::filesystem::path &bundlePath) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::createCatalogFragment() is not implemented");
    }

    AudioPluginExtensibility<AudioPluginFormat> * AudioPluginFormatLV2::Impl::getExtensibility() {
        return &extensibility;
    }

    AudioPluginFormatLV2::AudioPluginFormatLV2(std::vector<std::string> &overrideSearchPaths) {
        impl = new Impl(this);
    }

    AudioPluginFormatLV2::~AudioPluginFormatLV2() {
        delete impl;
    }

    AudioPluginExtensibility<AudioPluginFormat> * AudioPluginFormatLV2::getExtensibility() {
        return impl->getExtensibility();
    }

    std::vector<std::filesystem::path>& AudioPluginFormatLV2::getDefaultSearchPaths() {
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
            paths.append_range(defaultSearchPathsLV2);
            return paths;
        }();
        return ret;
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginFormatLV2::scanAllAvailablePlugins() {
        return impl->scanAllAvailablePlugins();
    }

    void AudioPluginFormatLV2::createInstance(PluginCatalogEntry *info,
        std::function<void(InvokeResult)> callback) {
        impl->createInstance(info, callback);
    }

    AudioPluginFormatLV2::Extensibility::Extensibility(AudioPluginFormat &format) :
        AudioPluginExtensibility(format) {
    }

    // AudioPluginInstanceLV2

    AudioPluginInstanceLV2::AudioPluginInstanceLV2(AudioPluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin) :
        formatImpl(formatImpl), plugin(plugin),
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
        lilv_instance_activate(instance);
        return StatusCode::OK;
    }

    StatusCode AudioPluginInstanceLV2::stopProcessing() {
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
}
