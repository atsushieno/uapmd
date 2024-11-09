#include "PluginFormatLV2.hpp"

namespace remidy {
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
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) {
        impl->createInstance(info, callback);
    }

    PluginFormatLV2::Extensibility::Extensibility(PluginFormat &format) :
        PluginExtensibility(format) {
    }

    // AudioPluginInstanceLV2

    AudioPluginInstanceLV2::AudioPluginInstanceLV2(PluginCatalogEntry* entry, PluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin) :
        PluginInstance(entry), formatImpl(formatImpl), plugin(plugin),
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

    PluginParameterSupport *AudioPluginInstanceLV2::parameters() {
        if (!_parameters)
            _parameters = new ParameterSupport(this);
        return _parameters;
    }

    // AudioPluginInstanceLV2::PluginParameterSupportLV2

    AudioPluginInstanceLV2::ParameterSupport::~ParameterSupport()  {
        for (auto h : parameter_handlers)
            delete h;
        for (auto p : parameter_defs)
            delete p;
    }

    std::vector<PluginParameter*> AudioPluginInstanceLV2::ParameterSupport::parameters() {
        return parameter_defs;
    }

    std::unique_ptr<PluginParameter> createParameter(const LilvNode* parameter, remidy_lv2::LV2ImplPluginContext& implContext, Logger* logger, std::string& displayName) {
        auto labelNode = lilv_world_get(implContext.world, parameter, implContext.statics->rdfs_label_node, nullptr);
        if (!labelNode) {
            logger->logError("A patch writable does not specify RDF label.");
            return nullptr;
        }
        auto label = std::string{lilv_node_as_string(labelNode)};

        // portGroup is used as its parameter path
        auto portGroupNode = lilv_world_get(implContext.world, parameter, implContext.statics->port_group_uri_node, nullptr);
        auto portGroup = portGroupNode ? std::string{lilv_node_as_string(portGroupNode)} : "";

        auto rangeNode = lilv_world_get(implContext.world, parameter, implContext.statics->rdfs_range_node, nullptr);
        if (rangeNode) {
            auto type = std::string{lilv_node_as_uri(rangeNode)};
            if (type == LV2_ATOM__Float ||
                type == LV2_ATOM__Double ||
                type == LV2_ATOM__Bool ||
                type == LV2_ATOM__Int ||
                type == LV2_ATOM__Long)
                ;// okay
            else if (type == LV2_ATOM__String ||
                     type == LV2_ATOM__Path ||
                     type == LV2_ATOM__URI) {
                logger->logInfo("%s: ATOM String, Path, and URI are ignored.", displayName.c_str());
                return nullptr;
            }
            else {
                logger->logError("%s: Unexpected ATOM type `%s`. Ignored.", displayName.c_str(), type.c_str());
                return nullptr;
            }
        }

        // There is no `lilv_node_as_double()` (no `LILV_VALUE_DOUBLE` either...)
        auto defValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->default_uri_node, nullptr);
        double defValue = defValueNode ? lilv_node_as_float(defValueNode) : 0;
        auto minValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->minimum_uri_node, nullptr);
        double minValue = defValueNode ? lilv_node_as_float(minValueNode) : 0;
        auto maxValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->maximum_uri_node, nullptr);
        double maxValue = defValueNode ? lilv_node_as_float(maxValueNode) : 0;

        std::vector<ParameterEnumeration> enums{};
        auto scalePoints = lilv_world_find_nodes(implContext.world, parameter, implContext.statics->scale_point_uri_node, nullptr);
        LILV_FOREACH(nodes, s, scalePoints) {
            auto sv = lilv_nodes_get(scalePoints, s);
            auto enumValueNode = lilv_world_get(implContext.world, sv,
                                                implContext.statics->rdf_value_node, nullptr);
            auto enumValue = enumValueNode ? lilv_node_as_float(enumValueNode) : 0;
            auto enumLabelNode = lilv_world_get(implContext.world, sv,
                                                implContext.statics->rdfs_label_node, nullptr);
            if (!enumLabelNode)
                // warn about missing label, but continue, it might be anonymous.
                logger->logWarning("%s: A scalePoint for `%s` misses its label at %f.",
                                   displayName.c_str(), label.c_str(), enumValue);
            auto enumLabel = std::string{lilv_node_as_string(enumLabelNode)};
            ParameterEnumeration pe{enumLabel, enumValue};
            enums.emplace_back(pe);
        }

        return std::make_unique<PluginParameter>(label, label, portGroup, defValue, minValue, maxValue, false, enums);
    }

    void AudioPluginInstanceLV2::ParameterSupport::inspectParameters() {
        auto formatImpl = owner->formatImpl;
        auto& implContext = owner->implContext;
        auto plugin = owner->plugin;
        auto& displayName = owner->info()->displayName();

        auto logger = formatImpl->worldContext->logger;

        std::map<const LilvNode*,std::unique_ptr<PluginParameter>> pl{};
        // this is what Ardour does: https://github.com/Ardour/ardour/blob/a76afae0e9ffa8a44311d6f9c1d8dbc613bfc089/libs/ardour/lv2_plugin.cc#L2142
        auto pluginSubject = lilv_plugin_get_uri(plugin);
        auto writables = lilv_world_find_nodes(formatImpl->world, pluginSubject, formatImpl->worldContext->patch_writable_uri_node, nullptr);
        LILV_FOREACH(nodes, iter, writables) {
            auto writable = lilv_nodes_get(writables, iter);
            auto parameter = createParameter(writable, implContext, logger, displayName);
            if (parameter)
                pl[writable] = std::move(parameter);
        }
        // iterate through readable patches. If there is a read-only parameter, add to the list.
        auto readables = lilv_world_find_nodes(formatImpl->world, pluginSubject, formatImpl->worldContext->patch_readable_uri_node, nullptr);
        LILV_FOREACH(nodes, iter, readables) {
            auto readable = lilv_nodes_get(writables, iter);
            if (pl.contains(readable))
                pl[readable]->readable(true);
            else {
                auto parameter = createParameter(readable, implContext, logger, displayName);
                if (parameter)
                    pl[readable] = std::move(parameter);
            }
        }

        for (auto& p : pl) {
            auto para = p.second.release();
            parameter_handlers.emplace_back(new LV2AtomParameterHandler(implContext, para));
            parameter_defs.emplace_back(para);
        }
    }

    StatusCode AudioPluginInstanceLV2::ParameterSupport::getParameter(uint32_t index, double *value) {
        return parameter_handlers[index]->getParameter(value);
    }

    StatusCode AudioPluginInstanceLV2::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
        return parameter_handlers[index]->setParameter(value, timestamp);
    }
}
