#include <atomic>

#include "remidy.hpp"
#include <lilv/lilv.h>

#include "lv2/LV2Helper.hpp"

namespace remidy {
    class AudioPluginScannerLV2 : public FileBasedPluginScanner {
        LilvWorld* world;
    public:
        explicit AudioPluginScannerLV2(LilvWorld* world) : world(world) {}

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
            uint32_t numEventIn{0};
            uint32_t numEventOut{0};
        };
        BusSearchResult buses;
        BusSearchResult inspectBuses();
        std::vector<AudioBusDefinition> input_bus_defs;
        std::vector<AudioBusDefinition> output_bus_defs;
        std::vector<AudioBusConfiguration*> input_buses;
        std::vector<AudioBusConfiguration*> output_buses;
        std::vector<void*> port_buffers{};

        struct RemidyToLV2PortMapping {
            size_t bus;
            uint32_t channel;
            int32_t lv2Port;
        };
        std::vector<RemidyToLV2PortMapping> audio_in_port_mapping{};
        std::vector<RemidyToLV2PortMapping> audio_out_port_mapping{};

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
        bool hasAudioInputs() override { return !input_buses.empty(); }
        bool hasAudioOutputs() override { return !output_buses.empty(); }
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
        if (instance) {
            lilv_instance_deactivate(instance);
            lilv_instance_free(instance);
        }
        instance = nullptr;
        if (plugin) {
            uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
            for (auto p : port_buffers)
                if (p)
                    free(p);
        }
    }

    bool getNextAudioPortIndex(remidy_lv2::LV2ImplPluginContext& ctx, const LilvPlugin* plugin, const bool isInput, int32_t& result, int32_t& lv2PortIndex, uint32_t numPorts) {
        while(lv2PortIndex < numPorts) {
            auto port = lilv_plugin_get_port_by_index(plugin, lv2PortIndex);
            if (isInput ? ctx.IS_AUDIO_IN(plugin, port) : ctx.IS_AUDIO_OUT(plugin, port)) {
                result = lv2PortIndex++;
                return true;
            }
            lv2PortIndex++;
        }
        result = -1;
        return false;
    }

    StatusCode AudioPluginInstanceLV2::configure(ConfigurationRequest& configuration) {
        // Do we have to deal with offlineMode? LV2 only mentions hardRT*Capable*.

        if (instance)
            // we need to save state delete instance, recreate instance with the
                // new configuration, and restore the state.
                    throw std::runtime_error("AudioPluginInstanceLV2::configure() re-configuration is not implemented");

        instance = instantiate_plugin(formatImpl->worldContext, &implContext, plugin,
            configuration.sampleRate, configuration.offlineMode);
        if (!instance)
            return StatusCode::FAILED_TO_INSTANTIATE;

        // create port mappings
        uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
        int32_t portToScan = 0;
        auto audioIns = audioInputBuses();
        int32_t lv2AudioInIdx = 0;
        for (size_t i = 0, n = audioIns.size(); i < n; i++) {
            auto bus = audioIns[i];
            for (uint32_t ch = 0, nCh = bus->channelLayout().channels(); ch < nCh; ch++) {
                getNextAudioPortIndex(implContext, plugin, true, lv2AudioInIdx, portToScan, numPorts);
                audio_in_port_mapping.emplace_back(RemidyToLV2PortMapping{.bus = i, .channel = ch, .lv2Port = lv2AudioInIdx});
            }
        }
        portToScan = 0;
        const auto audioOuts = audioOutputBuses();
        int32_t lv2AudioOutIdx = 0;
        for (size_t i = 0, n = audioOuts.size(); i < n; i++) {
            const auto bus = audioOuts[i];
            for (uint32_t ch = 0, nCh = bus->channelLayout().channels(); ch < nCh; ch++) {
                getNextAudioPortIndex(implContext, plugin, false, lv2AudioOutIdx, portToScan, numPorts);
                audio_out_port_mapping.emplace_back(RemidyToLV2PortMapping{.bus = i, .channel = ch, .lv2Port = lv2AudioOutIdx});
            }
        }

        for (const auto p : port_buffers)
            if (p)
                free(p);
        port_buffers.clear();
        for (int i = 0; i < numPorts; i++) {
            if (const auto port = lilv_plugin_get_port_by_index(plugin, i);
                !implContext.IS_AUDIO_PORT(plugin, port)) {
                const LilvNode* minSizeNode = lilv_port_get(plugin, port, implContext.statics->resize_port_minimum_size_node);
                const int minSize = minSizeNode ? lilv_node_as_int(minSizeNode) : 0;
                auto buffer = calloc(minSize ? minSize : sizeof(float), 1);
                port_buffers.emplace_back(buffer);
                lilv_instance_connect_port(instance, i, buffer);
            }
            else
                port_buffers.emplace_back(nullptr);
        }

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
        for (auto& m : audio_in_port_mapping) {
            auto audioIn = process.audioIn(m.bus)->getFloatBufferForChannel(m.channel);
            lilv_instance_connect_port(instance, m.lv2Port, audioIn);
        }
        for (auto& m : audio_out_port_mapping) {
            auto audioOut = process.audioOut(m.bus)->getFloatBufferForChannel(m.channel);
            lilv_instance_connect_port(instance, m.lv2Port, audioOut);
        }

        // FIXME: process Atom inputs

        lilv_instance_run(instance, process.frameCount());

        // FIXME: process Atom outputs

        return StatusCode::OK;
    }

    AudioPluginInstanceLV2::BusSearchResult AudioPluginInstanceLV2::inspectBuses() {
        BusSearchResult ret{};

        input_bus_defs.clear();
        output_bus_defs.clear();
        input_buses.clear();
        output_buses.clear();
        for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
            auto port = lilv_plugin_get_port_by_index(plugin, p);
            if (implContext.IS_AUDIO_PORT(plugin, port)) {
                bool isInput = implContext.IS_INPUT_PORT(plugin, port);
                auto groupNode = lilv_port_get(plugin, port, implContext.statics->port_group_uri_node);
                std::string group = groupNode == nullptr ? "" : lilv_node_as_string(groupNode);
                auto scNode = lilv_port_get(plugin, port, implContext.statics->is_side_chain_uri_node);
                bool isSideChain = scNode != nullptr && lilv_node_as_bool(scNode);
                std::optional<AudioBusDefinition> def{};
                int32_t index = 0;
                for (auto d : isInput ? input_bus_defs : output_bus_defs) {
                    if (d.name() == group) {
                        def = d;
                        break;
                    }
                    index++;
                }
                if (!def.has_value()) {
                    def = AudioBusDefinition(group, isSideChain ? AudioBusRole::Aux : AudioBusRole::Main);
                    (isInput ? input_bus_defs : output_bus_defs).emplace_back(def.value());
                    auto bus = new AudioBusConfiguration(def.value());
                    bus->channelLayout(AudioChannelLayout::mono());
                    (isInput ? input_buses : output_buses).emplace_back(bus);
                } else {
                    auto bus = (isInput ? input_buses : output_buses)[index];
                    if (bus->channelLayout() != AudioChannelLayout::mono())
                        bus->channelLayout(AudioChannelLayout::stereo());
                    else
                        throw std::runtime_error{"Audio ports more than stereo channels are not supported yet."};
                }
            }
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
