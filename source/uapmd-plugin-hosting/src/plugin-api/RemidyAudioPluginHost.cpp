#include "RemidyAudioPluginHost.hpp"
#include "RemidyAudioPluginInstance.hpp"
#include <functional>
#include <ranges>
#if ANDROID
#include <android/log.h>
#endif
#if _WIN32
#include <Windows.h>
#endif
#ifdef __EMSCRIPTEN__
#include "../../../remidy/src/webclap/PluginFormatWebCLAP.hpp"
#endif
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_plugin_hosting {
    int32_t instanceIdSerial{0};

#ifdef __EMSCRIPTEN__
    bool trySendWebClapInputEvents(AudioPluginInstanceAPI* instance, const uapmd_ump_t* events, size_t sizeInBytes) {
        auto* remidyInstance = dynamic_cast<RemidyAudioPluginInstance*>(instance);
        return remidyInstance && remidyInstance->trySendWebClapInputEvents(events, sizeInBytes);
    }
#endif
}

std::unique_ptr<uapmd_plugin_hosting::AudioPluginHostingAPI> uapmd_plugin_hosting::AudioPluginHostingAPI::create() {
    return std::make_unique<RemidyAudioPluginHost>();
}

uapmd_plugin_hosting::RemidyAudioPluginHost::RemidyAudioPluginHost() {
    scanning = uapmd_plugin_hosting::PluginScanTool::create();
#if ANDROID
    // Android has no persistent plugin cache path here. Populate fast-scan entries
    // eagerly so formats like AAP appear in the catalog without a manual scan.
    scanning->performPluginScanning(true, uapmd_plugin_hosting::ScanMode::InProcess, false);
#endif
#if _WIN32
    // VST3 plugins (especially NI and JUCE-based ones) use COM and require STA.
    // Initialize COM before any DLL loading so initDll / GetPluginFactory run
    // inside a properly-initialized apartment.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr))
        comInitialized = true;
    else if (hr == RPC_E_CHANGED_MODE)
        remidy::Logger::global()->logWarning("RemidyAudioPluginHost: COM already initialized with a different apartment model; VST3 plugins using COM (e.g. NI) may crash");
#endif
}

uapmd_plugin_hosting::RemidyAudioPluginHost::~RemidyAudioPluginHost() {
#if _WIN32
    if (comInitialized)
        CoUninitialize();
#endif
}

std::vector<uapmd_plugin_hosting::AudioPluginCatalogEntry> uapmd_plugin_hosting::RemidyAudioPluginHost::pluginCatalogEntries() {
    std::vector<AudioPluginCatalogEntry> ret{};
    for (const auto e : scanning->catalog().getPlugins())
        ret.emplace_back(*e);
    return ret;
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::savePluginCatalogToFile(std::filesystem::path path) {
    scanning->catalog().save(path);
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::addPluginFormat(AudioPluginFormat* format) {
    scanning->addFormat(format);
}

std::vector<uapmd_plugin_hosting::AudioPluginFormat*> uapmd_plugin_hosting::RemidyAudioPluginHost::pluginFormats() {
    return scanning->formats();
}

std::filesystem::path empty_path{""};
void uapmd_plugin_hosting::RemidyAudioPluginHost::performPluginScanning(bool rescan) {
    scanning->catalog().clear();
    scanning->performPluginScanning(false, uapmd_plugin_hosting::ScanMode::InProcess, rescan);
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::reloadPluginCatalogFromCache() {
    auto& cacheFile = scanning->pluginListCacheFile();
    if (cacheFile.empty()) {
        scanning->catalog().clear();
        scanning->performPluginScanning(true, uapmd_plugin_hosting::ScanMode::InProcess, false);
        return;
    }

    scanning->catalog().clear();
    scanning->performPluginScanning(true, uapmd_plugin_hosting::ScanMode::InProcess, false);
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::createPluginInstance(uint32_t sampleRate,
                                                        uint32_t bufferSize,
                                                        std::optional<uint32_t> mainInputChannels,
                                                        std::optional<uint32_t> mainOutputChannels,
                                                        bool offlineMode,
                                                        std::string &formatName,
                                                        std::string &pluginId,
                                                        std::function<void(int32_t instanceId, std::string error)>&& callback) {
    auto formats = scanning->formats();
    auto formatIt = std::ranges::find_if(formats, [&formatName](auto f) { return f->name() == formatName; });
    if (formatIt == formats.end()) {
        callback(-1, "Plugin format not found: " + formatName);
        return;
    }
    auto format = *formatIt;
    auto plugins = scanning->catalog().getPlugins();
    auto entry = std::ranges::find_if(plugins, [&formatName,&pluginId](auto e) {
        return e->format() == formatName && e->pluginId() == pluginId;
    });
    if (entry == plugins.end())
        callback(-1, "Plugin not found");
    else {
        auto instancing = std::make_shared<PluginInstancing>(*scanning, format, *entry);
        auto& configuration = instancing->configurationRequest();
        configuration.sampleRate = sampleRate;
        configuration.bufferSizeInSamples = bufferSize;
        configuration.offlineMode = offlineMode;
        configuration.mainInputChannels = mainInputChannels;
        configuration.mainOutputChannels = mainOutputChannels;
        auto cb = std::move(callback);
        instancing->makeAlive([this,instancing,cb](std::string error) {
            if (error.empty())
                instancing->withInstance([this,instancing,cb](AudioPluginInstanceAPI* instance) {
                    auto instanceId = instanceIdSerial++;
                    // Only formats whose plugins can change their own state register this.
                    if (auto* stateChange = dynamic_cast<PluginStateChangeExtension*>(
                            instance->extension(kPluginStateChangeExtensionId)))
                        stateChange->onPluginStateChanged([this, instanceId] {
                            plugin_state_change_event_.notify(instanceId);
                        });
                    // The instance stays owned by the PluginInstancing that drives its
                    // lifecycle, which is kept alive by this entry in the instance map.
                    instances[instanceId] = instancing;
                    cb(instanceId, "");
                });
            else {
                cb(-1, error);
            }
        });
    }
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::deletePluginInstance(int32_t instanceId) {
    instances.erase(instanceId);
}
std::vector<int32_t> uapmd_plugin_hosting::RemidyAudioPluginHost::instanceIds() {
    std::vector<int32_t> ret;
    for (auto& i : instances)
        ret.push_back(i.first);
    return ret;
}

uapmd_plugin_hosting::AudioPluginInstanceAPI * uapmd_plugin_hosting::RemidyAudioPluginHost::getInstance(int32_t instanceId) {
    auto it = instances.find(instanceId);
    return it == instances.end() || !it->second ? nullptr : it->second->instance();
}

remidy::EventListenerId uapmd_plugin_hosting::RemidyAudioPluginHost::addPluginStateChangeListener(std::function<void(int32_t)> listener) {
    return plugin_state_change_event_.addListener(std::move(listener));
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::removePluginStateChangeListener(remidy::EventListenerId listenerId) {
    plugin_state_change_event_.removeListener(listenerId);
}

void uapmd_plugin_hosting::RemidyAudioPluginHost::onTrackGraphNodeAdded(int32_t instanceId, int32_t trackIndex, bool isMasterTrack, uint32_t order) {
#ifdef __EMSCRIPTEN__
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second)
        return;
    auto* remidy_instance = dynamic_cast<RemidyAudioPluginInstance*>(it->second->instance());
    if (!remidy_instance)
        return;
    auto* webclap_instance = dynamic_cast<remidy::PluginInstanceWebCLAP*>(remidy_instance->rawInstance());
    if (!webclap_instance)
        return;
    webclap_instance->attachToTrackGraph(trackIndex, isMasterTrack, order);
#else
    (void) instanceId;
    (void) trackIndex;
    (void) isMasterTrack;
    (void) order;
#endif
}
