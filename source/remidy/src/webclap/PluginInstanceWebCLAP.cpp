#include "PluginInstanceWebCLAP.hpp"

#if !defined(__EMSCRIPTEN__)

namespace remidy {}

#else

#include <algorithm>
#include <string_view>
#include <utility>

#include <emscripten.h>

namespace {
    std::string normalizeWebclapId(const std::string& id) {
        constexpr std::string_view prefix{"wclap:"};
        if (id.rfind(prefix.data(), 0) == 0) {
            return id.substr(prefix.size());
        }
        return id;
    }

    void registerFunctionLabel(uint32_t tableIndex, const char* label) {
#if defined(__EMSCRIPTEN__)
        if (tableIndex == 0 || label == nullptr)
            return;
        EM_ASM({
            if (globalThis.uapmdWebClap && typeof globalThis.uapmdWebClap.registerFunctionLabel === 'function') {
                globalThis.uapmdWebClap.registerFunctionLabel($0 >>> 0, UTF8ToString($1));
            }
        }, tableIndex, label);
#else
        (void)tableIndex;
        (void)label;
#endif
    }
}

namespace remidy {

namespace {
    thread_local PluginInstanceWebCLAP* g_jsBridgeInstance = nullptr;
    thread_local AudioProcessContext* g_jsBridgeContext = nullptr;
    thread_local uint32_t g_jsBridgeHandle = 0;
}

PluginInstanceWebCLAP::PluginInstanceWebCLAP(PluginCatalogEntry* entry,
                                             std::unique_ptr<WebclapSdkInstance> sdkInstance,
                                             std::string cacheRoot)
    : PluginInstance(entry),
      instance_(std::move(sdkInstance)),
      cacheRoot_(std::move(cacheRoot)) {}

PluginUIThreadRequirement PluginInstanceWebCLAP::requiresUIThreadOn() {
    return PluginUIThreadRequirement::None;
}

PluginAudioBuses* PluginInstanceWebCLAP::audioBuses() {
    return nullptr;
}

PluginParameterSupport* PluginInstanceWebCLAP::parameters() {
    return nullptr;
}

PluginStateSupport* PluginInstanceWebCLAP::states() {
    return nullptr;
}

PluginPresetsSupport* PluginInstanceWebCLAP::presets() {
    return nullptr;
}

PluginUISupport* PluginInstanceWebCLAP::ui() {
    return nullptr;
}

bool PluginInstanceWebCLAP::requiresReplacingProcess() const {
    return false;
}

const std::string& PluginInstanceWebCLAP::cacheRoot() const {
    return cacheRoot_;
}

uint32_t PluginInstanceWebCLAP::webHandle() const {
    return handleValue();
}

bool PluginInstanceWebCLAP::processFromBridge(AudioProcessContext& context, int32_t frames) {
    return invokeProcess(context, frames);
}

uint32_t PluginInstanceWebCLAP::handleValue() const {
    return instance_ ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(instance_.get())) : 0u;
}

    class JsProcessScope {
    public:
        JsProcessScope(PluginInstanceWebCLAP* instance, AudioProcessContext& context)
            : previousInstance_(g_jsBridgeInstance),
              previousContext_(g_jsBridgeContext),
              previousHandle_(g_jsBridgeHandle) {
            g_jsBridgeInstance = instance;
            g_jsBridgeContext = &context;
            g_jsBridgeHandle = instance ? instance->webHandle() : 0;
        }
        ~JsProcessScope() {
            g_jsBridgeInstance = previousInstance_;
            g_jsBridgeContext = previousContext_;
            g_jsBridgeHandle = previousHandle_;
        }

    private:
        PluginInstanceWebCLAP* previousInstance_{};
        AudioProcessContext* previousContext_{};
        uint32_t previousHandle_{};
    };
}
#endif

#if defined(__EMSCRIPTEN__)
namespace remidy {

    PluginInstanceWebCLAP::~PluginInstanceWebCLAP() {
        destroyPlugin();
        if (entryInitialized_ && instance_ && !instance_->is64()) {
            if (entryCopy_.deinit.wasmPointer != 0) {
                instance_->call(entryCopy_.deinit);
            }
            entryInitialized_ = false;
        }
    }

    bool PluginInstanceWebCLAP::invokeConfigure(uint32_t sampleRate,
                                                uint32_t bufferSize,
                                                uint32_t mainInputs,
                                                uint32_t mainOutputs) {
        if (!ensureProcessStorage(mainInputs, mainOutputs, bufferSize))
            return false;
        auto pluginPtr = pluginPtr_.template cast<const wclap32::wclap_plugin>();
        if (!pluginPtr)
            return false;
        auto minFrames = bufferSize == 0 ? 1u : bufferSize;
        auto maxFrames = bufferSize == 0 ? 1u : bufferSize;
        return instance_->call(plugin_.activate,
                               pluginPtr,
                               static_cast<double>(sampleRate),
                               minFrames,
                               maxFrames);
    }

    bool PluginInstanceWebCLAP::invokeStart() {
        auto pluginPtr = pluginPtr_.template cast<const wclap32::wclap_plugin>();
        if (!pluginPtr)
            return false;
        return instance_->call(plugin_.start_processing, pluginPtr);
    }

    bool PluginInstanceWebCLAP::invokeStop() {
        auto pluginPtr = pluginPtr_.template cast<const wclap32::wclap_plugin>();
        if (!pluginPtr)
            return false;
        instance_->call(plugin_.stop_processing, pluginPtr);
        return true;
    }

    bool PluginInstanceWebCLAP::invokeProcess(AudioProcessContext& context, int32_t frameCount) {
        if (frameCount <= 0)
            return true;
        auto frames = static_cast<uint32_t>(frameCount);
        if (frames > processBufferCapacity_) {
            if (!ensureProcessStorage(inputPort_.channelCount, outputPort_.channelCount, frames))
                return false;
        }
        if (!copyInputsToRemote(context, frames))
            return false;
        processCopy_.frames_count = frameCount;
        processCopy_.steady_time = -1;
        instance_->set(processPtr_, processCopy_);
        auto pluginPtr = pluginPtr_.template cast<const wclap32::wclap_plugin>();
        if (!pluginPtr)
            return false;
        auto status = instance_->call(plugin_.process,
                                      pluginPtr,
                                      processPtr_.template cast<const wclap32::wclap_process>());
        if (status == wclap32::WCLAP_PROCESS_ERROR)
            return false;
        return copyOutputsFromRemote(context, frames);
    }

    StatusCode PluginInstanceWebCLAP::configure(ConfigurationRequest& configuration) {
        if (!ensurePlugin())
            return StatusCode::FAILED_TO_CONFIGURE;
        lastConfiguration_ = configuration;
        auto mainInputs = configuration.mainInputChannels.value_or(2u);
        auto mainOutputs = configuration.mainOutputChannels.value_or(2u);
        if (!invokeConfigure(configuration.sampleRate, configuration.bufferSizeInSamples, mainInputs, mainOutputs)) {
            Logger::global()->logWarning("[WebCLAP] configureInstance failed for %s", this->info()->displayName().c_str());
            return StatusCode::FAILED_TO_CONFIGURE;
        }
        configured_ = true;
#if defined(__EMSCRIPTEN__)
        EM_ASM({
            if (globalThis.uapmdWebClap && typeof globalThis.uapmdWebClap.configureInstance === 'function') {
                globalThis.uapmdWebClap.configureInstance($0, $1, $2, $3, $4);
            }
        }, webHandle(), configuration.sampleRate, configuration.bufferSizeInSamples, mainInputs, mainOutputs);
#endif
        return StatusCode::OK;
    }

    StatusCode PluginInstanceWebCLAP::startProcessing() {
        if (!configured_)
            return StatusCode::ALREADY_INVALID_STATE;
        if (!ensurePlugin())
            return StatusCode::FAILED_TO_START_PROCESSING;
        if (!invokeStart()) {
            Logger::global()->logWarning("[WebCLAP] startInstance failed for %s", this->info()->displayName().c_str());
            return StatusCode::FAILED_TO_START_PROCESSING;
        }
        running_ = true;
#if defined(__EMSCRIPTEN__)
        EM_ASM({
            if (globalThis.uapmdWebClap && typeof globalThis.uapmdWebClap.startInstance === 'function') {
                globalThis.uapmdWebClap.startInstance($0);
            }
        }, webHandle());
#endif
        return StatusCode::OK;
    }

    StatusCode PluginInstanceWebCLAP::stopProcessing() {
        if (!running_)
            return StatusCode::ALREADY_INVALID_STATE;
        if (!ensurePlugin())
            return StatusCode::FAILED_TO_STOP_PROCESSING;
        if (!invokeStop()) {
            Logger::global()->logWarning("[WebCLAP] stopInstance failed for %s", this->info()->displayName().c_str());
            return StatusCode::FAILED_TO_STOP_PROCESSING;
        }
        running_ = false;
#if defined(__EMSCRIPTEN__)
        EM_ASM({
            if (globalThis.uapmdWebClap && typeof globalThis.uapmdWebClap.stopInstance === 'function') {
                globalThis.uapmdWebClap.stopInstance($0);
            }
        }, webHandle());
#endif
        return StatusCode::OK;
    }

    StatusCode PluginInstanceWebCLAP::process(AudioProcessContext& context) {
        if (!running_)
            return StatusCode::ALREADY_INVALID_STATE;
        if (!ensurePlugin())
            return StatusCode::FAILED_TO_PROCESS;
        auto frames = std::max(context.frameCount(), 0);
#if defined(__EMSCRIPTEN__)
        int32_t jsStatus = -1;
        {
            JsProcessScope scope(this, context);
            jsStatus = EM_ASM_INT({
                if (globalThis.uapmdWebClap && typeof globalThis.uapmdWebClap.processInstance === 'function') {
                    return globalThis.uapmdWebClap.processInstance($0, $1) | 0;
                }
                return -1;
            }, webHandle(), frames);
        }
        if (jsStatus == 1)
            return StatusCode::OK;
        if (jsStatus == 0)
            return StatusCode::FAILED_TO_PROCESS;
#endif
        if (!invokeProcess(context, frames))
            return StatusCode::NOT_IMPLEMENTED;
        return StatusCode::OK;
    }

    wclap32::Pointer<char> PluginInstanceWebCLAP::writeString(const std::string& value) {
        if (!instance_ || instance_->is64())
            return {};
        auto length = static_cast<uint32_t>(value.size() + 1);
        auto remote = instance_->malloc32(length).template cast<char>();
        if (!remote)
            return {};
        instance_->setArray(remote, value.c_str(), length);
        pinnedStrings_.push_back(remote);
        return remote;
    }

    std::string PluginInstanceWebCLAP::readString(wclap32::Pointer<const char> value) {
        if (!instance_ || instance_->is64() || !value)
            return {};
        constexpr uint32_t kMaxString = 4096u;
        auto length = instance_->countUntil(wclap32::Pointer<char>{value.wasmPointer}, char{0}, kMaxString);
        if (length == 0)
            return {};
        std::string result(length, '\0');
        instance_->getArray(value, result.data(), length);
        return result;
    }

    bool PluginInstanceWebCLAP::ensureEntryInitialized() {
        if (entryInitialized_)
            return true;
        if (!instance_) {
            Logger::global()->logError("[WebCLAP] Instance handle is not available.");
            return false;
        }
        if (instance_->is64()) {
            Logger::global()->logError("[WebCLAP] wasm64 WebCLAP modules are not supported yet.");
            return false;
        }
        if (!ensureHostStruct())
            return false;
        if (!sdkInitialized_) {
            instance_->init();
            sdkInitialized_ = true;
        }
        auto entryPtr = instance_->entry32;
        if (!entryPtr) {
            Logger::global()->logError("[WebCLAP] Plugin entry pointer is missing.");
            return false;
        }
        entryCopy_ = instance_->get(entryPtr);
        registerFunctionLabel(entryCopy_.init.wasmPointer, "clap_entry.init");
        registerFunctionLabel(entryCopy_.deinit.wasmPointer, "clap_entry.deinit");
        registerFunctionLabel(entryCopy_.get_factory.wasmPointer, "clap_entry.get_factory");
        if (!pluginPathPtr_) {
            auto pluginPath = cacheRoot_.empty() ? std::string{"/"} : cacheRoot_;
            pluginPathPtr_ = writeString(pluginPath);
        }
        if (!pluginPathPtr_) {
            Logger::global()->logError("[WebCLAP] Failed to allocate plugin path for init().");
            return false;
        }
        auto pathPtr = pluginPathPtr_.template cast<const char>();
        if (!instance_->call(entryCopy_.init, pathPtr)) {
            Logger::global()->logError("[WebCLAP] clap_entry.init() failed for %s", this->info()->displayName().c_str());
            return false;
        }
        entryInitialized_ = true;
        return true;
    }

    bool PluginInstanceWebCLAP::ensureFactory() {
        if (factoryPtr_)
            return true;
        if (!ensureEntryInitialized())
            return false;
        constexpr const char* kFactoryId = "clap.plugin-factory";
        auto factoryIdPtr = writeString(kFactoryId);
        if (!factoryIdPtr) {
            Logger::global()->logError("[WebCLAP] Failed to allocate factory id string.");
            return false;
        }
        auto result = instance_->call(entryCopy_.get_factory, factoryIdPtr.template cast<const char>());
        if (!result.wasmPointer) {
            Logger::global()->logError("[WebCLAP] clap_entry.get_factory() returned null.");
            return false;
        }
        factoryPtr_ = result.template cast<const wclap32::wclap_plugin_factory>();
        factoryCopy_ = instance_->get(factoryPtr_);
        registerFunctionLabel(factoryCopy_.get_plugin_count.wasmPointer, "clap_factory.get_plugin_count");
        registerFunctionLabel(factoryCopy_.get_plugin_descriptor.wasmPointer, "clap_factory.get_plugin_descriptor");
        registerFunctionLabel(factoryCopy_.create_plugin.wasmPointer, "clap_factory.create_plugin");
        return true;
    }

    bool PluginInstanceWebCLAP::ensureHostStruct() {
        if (hostPtr_)
            return true;
        if (!instance_ || instance_->is64())
            return false;
        wclap32::wclap_host host{};
        host.wclap_version = wclap32::WCLAP_VERSION;
        auto namePtr = writeString("remidy");
        host.name = namePtr.template cast<const char>();
        auto vendorPtr = writeString("uapmd");
        host.vendor = vendorPtr.template cast<const char>();
        auto urlPtr = writeString("https://github.com/atsushieno/uapmd");
        host.url = urlPtr.template cast<const char>();
        auto versionPtr = writeString("0.1");
        host.version = versionPtr.template cast<const char>();
        host.host_data = {};
        host.get_extension = instance_->registerHost32<wclap32::Pointer<const void>,
                                                       wclap32::Pointer<const wclap32::wclap_host>,
                                                       wclap32::Pointer<const char>>(this, &PluginInstanceWebCLAP::hostGetExtension);
        host.request_restart = instance_->registerHost32<void,
                                                         wclap32::Pointer<const wclap32::wclap_host>>(this, &PluginInstanceWebCLAP::hostRequestRestart);
        host.request_process = instance_->registerHost32<void,
                                                         wclap32::Pointer<const wclap32::wclap_host>>(this, &PluginInstanceWebCLAP::hostRequestProcess);
        host.request_callback = instance_->registerHost32<void,
                                                          wclap32::Pointer<const wclap32::wclap_host>>(this, &PluginInstanceWebCLAP::hostRequestCallback);
        hostStruct_ = host;
        hostPtr_ = instance_->malloc32(sizeof(wclap32::wclap_host)).template cast<wclap32::wclap_host>();
        if (!hostPtr_) {
            Logger::global()->logError("[WebCLAP] Failed to allocate remote host struct.");
            return false;
        }
        instance_->set(hostPtr_, hostStruct_);
        if (!ensureHostState())
            return false;
        if (!ensureHostParams())
            return false;
        if (!ensureHostAudioPorts())
            return false;
        if (!ensureHostGui())
            return false;
        return true;
    }

    bool PluginInstanceWebCLAP::ensureHostState() {
        if (hostExtensions_.statePtr_)
            return true;
        hostExtensions_.state.mark_dirty =
            instance_->registerHost32<void, wclap32::Pointer<const wclap32::wclap_host>>(this,
                                                                                        &PluginInstanceWebCLAP::hostStateMarkDirty);
        auto ptr = instance_->malloc32(sizeof(wclap32::wclap_host_state)).template cast<wclap32::wclap_host_state>();
        if (!ptr)
            return false;
        instance_->set(ptr, hostExtensions_.state);
        hostExtensions_.statePtr_ = ptr;
        return true;
    }

    bool PluginInstanceWebCLAP::ensureHostParams() {
        if (hostExtensions_.paramsPtr_)
            return true;
        hostExtensions_.params.rescan = instance_->registerHost32<void,
                                                                  wclap32::Pointer<const wclap32::wclap_host>,
                                                                  uint32_t>(this,
                                                                            &PluginInstanceWebCLAP::hostParamsRescan);
        hostExtensions_.params.clear = instance_->registerHost32<void,
                                                                 wclap32::Pointer<const wclap32::wclap_host>,
                                                                 wclap32::wclap_id,
                                                                 uint32_t>(this,
                                                                           &PluginInstanceWebCLAP::hostParamsClear);
        hostExtensions_.params.request_flush =
            instance_->registerHost32<void, wclap32::Pointer<const wclap32::wclap_host>>(this,
                                                                                        &PluginInstanceWebCLAP::hostParamsRequestFlush);
        auto ptr = instance_->malloc32(sizeof(wclap32::wclap_host_params)).template cast<wclap32::wclap_host_params>();
        if (!ptr)
            return false;
        instance_->set(ptr, hostExtensions_.params);
        hostExtensions_.paramsPtr_ = ptr;
        return true;
    }

    bool PluginInstanceWebCLAP::ensureHostAudioPorts() {
        if (hostExtensions_.audioPortsPtr_)
            return true;
        hostExtensions_.audioPorts.is_rescan_flag_supported =
            instance_->registerHost32<bool, wclap32::Pointer<const wclap32::wclap_host>, uint32_t>(
                this, &PluginInstanceWebCLAP::hostAudioPortsIsRescanSupported);
        hostExtensions_.audioPorts.rescan =
            instance_->registerHost32<void, wclap32::Pointer<const wclap32::wclap_host>, uint32_t>(
                this, &PluginInstanceWebCLAP::hostAudioPortsRescan);
        auto ptr =
            instance_->malloc32(sizeof(wclap32::wclap_host_audio_ports)).template cast<wclap32::wclap_host_audio_ports>();
        if (!ptr)
            return false;
        instance_->set(ptr, hostExtensions_.audioPorts);
        hostExtensions_.audioPortsPtr_ = ptr;
        return true;
    }

    bool PluginInstanceWebCLAP::ensureHostGui() {
        if (hostExtensions_.guiPtr_)
            return true;
        hostExtensions_.gui.resize_hints_changed =
            instance_->registerHost32<void, wclap32::Pointer<const wclap32::wclap_host>>(
                this, &PluginInstanceWebCLAP::hostGuiResizeHintsChanged);
        hostExtensions_.gui.request_resize =
            instance_->registerHost32<bool, wclap32::Pointer<const wclap32::wclap_host>, uint32_t, uint32_t>(
                this, &PluginInstanceWebCLAP::hostGuiRequestResize);
        hostExtensions_.gui.request_show =
            instance_->registerHost32<bool, wclap32::Pointer<const wclap32::wclap_host>>(
                this, &PluginInstanceWebCLAP::hostGuiRequestShow);
        hostExtensions_.gui.request_hide =
            instance_->registerHost32<bool, wclap32::Pointer<const wclap32::wclap_host>>(
                this, &PluginInstanceWebCLAP::hostGuiRequestHide);
        hostExtensions_.gui.closed =
            instance_->registerHost32<void, wclap32::Pointer<const wclap32::wclap_host>, bool>(
                this, &PluginInstanceWebCLAP::hostGuiClosed);
        auto ptr = instance_->malloc32(sizeof(wclap32::wclap_host_gui)).template cast<wclap32::wclap_host_gui>();
        if (!ptr)
            return false;
        instance_->set(ptr, hostExtensions_.gui);
        hostExtensions_.guiPtr_ = ptr;
        return true;
    }

    bool PluginInstanceWebCLAP::ensurePlugin() {
        if (pluginPtr_)
            return true;
        if (!ensureFactory() || !ensureHostStruct())
            return false;
        auto pluginCount = instance_->call(factoryCopy_.get_plugin_count, factoryPtr_);
        if (pluginCount == 0) {
            Logger::global()->logError("[WebCLAP] Plugin factory reports zero plugins.");
            return false;
        }
        auto requestedId = this->info()->pluginId();
        auto normalizedRequestedId = normalizeWebclapId(requestedId);
        descriptorPtr_ = {};
        std::string descriptorMatchId;
        for (uint32_t index = 0; index < pluginCount; ++index) {
            auto descriptorPtr = instance_->call(factoryCopy_.get_plugin_descriptor, factoryPtr_, index);
            if (!descriptorPtr.wasmPointer)
                continue;
            auto descriptor = descriptorPtr.template cast<const wclap32::wclap_plugin_descriptor>();
            auto remoteDescriptor = instance_->get(descriptor);
            auto descriptorId = readString(remoteDescriptor.id);
            auto normalizedDescriptorId = normalizeWebclapId(descriptorId);
            if (descriptorId == requestedId || normalizedDescriptorId == normalizedRequestedId) {
                descriptorPtr_ = descriptor;
                descriptorMatchId = descriptorId;
                break;
            }
        }
        if (!descriptorPtr_) {
            Logger::global()->logError("[WebCLAP] Plugin factory did not expose %s", requestedId.c_str());
            return false;
        }
        auto pluginIdForFactory = descriptorMatchId.empty() ? requestedId : descriptorMatchId;
        auto pluginIdPtr = writeString(pluginIdForFactory);
        if (!pluginIdPtr) {
            Logger::global()->logError("[WebCLAP] Failed to allocate plugin id string.");
            return false;
        }
        auto created = instance_->call(factoryCopy_.create_plugin,
                                       factoryPtr_,
                                       hostPtr_.template cast<const wclap32::wclap_host>(),
                                       pluginIdPtr.template cast<const char>());
        if (!created.wasmPointer) {
            Logger::global()->logError("[WebCLAP] Factory failed to create plugin %s", requestedId.c_str());
            return false;
        }
        pluginPtr_ = created.template cast<const wclap32::wclap_plugin>();
        plugin_ = instance_->get(pluginPtr_);
        registerFunctionLabel(plugin_.init.wasmPointer, "clap_plugin.init");
        registerFunctionLabel(plugin_.destroy.wasmPointer, "clap_plugin.destroy");
        registerFunctionLabel(plugin_.activate.wasmPointer, "clap_plugin.activate");
        registerFunctionLabel(plugin_.deactivate.wasmPointer, "clap_plugin.deactivate");
        registerFunctionLabel(plugin_.start_processing.wasmPointer, "clap_plugin.start_processing");
        registerFunctionLabel(plugin_.stop_processing.wasmPointer, "clap_plugin.stop_processing");
        registerFunctionLabel(plugin_.process.wasmPointer, "clap_plugin.process");
        if (!instance_->call(plugin_.init, pluginPtr_)) {
            Logger::global()->logError("[WebCLAP] Plugin init() failed for %s", requestedId.c_str());
            destroyPlugin();
            return false;
        }
        pluginInitialized_ = true;
        return true;
    }

    void PluginInstanceWebCLAP::destroyPlugin() {
        if (!instance_ || instance_->is64())
            return;
        if (pluginPtr_.wasmPointer != 0) {
            plugin_ = instance_->get(pluginPtr_);
            if (pluginInitialized_ && plugin_.deactivate.wasmPointer != 0) {
                instance_->call(plugin_.deactivate, pluginPtr_);
            }
            if (plugin_.destroy.wasmPointer != 0) {
                instance_->call(plugin_.destroy, pluginPtr_);
            }
        }
        pluginPtr_ = {};
        descriptorPtr_ = {};
        pluginInitialized_ = false;
    }

    bool PluginInstanceWebCLAP::ensureProcessStruct() {
        return pluginPtr_.wasmPointer != 0;
    }

    wclap32::Pointer<const void> PluginInstanceWebCLAP::hostGetExtension(void* ctx,
                                                                         wclap32::Pointer<const wclap32::wclap_host>,
                                                                         wclap32::Pointer<const char> id) {
        auto* self = static_cast<PluginInstanceWebCLAP*>(ctx);
        if (!self)
            return {};
        auto extensionId = self->readString(id);
        if (extensionId == wclap32::WCLAP_EXT_STATE) {
            if (self->ensureHostState())
                return self->hostExtensions_.statePtr_.template cast<const void>();
        } else if (extensionId == wclap32::WCLAP_EXT_PARAMS) {
            if (self->ensureHostParams())
                return self->hostExtensions_.paramsPtr_.template cast<const void>();
        } else if (extensionId == wclap32::WCLAP_EXT_AUDIO_PORTS) {
            if (self->ensureHostAudioPorts())
                return self->hostExtensions_.audioPortsPtr_.template cast<const void>();
        } else if (extensionId == wclap32::WCLAP_EXT_GUI) {
            if (self->ensureHostGui())
                return self->hostExtensions_.guiPtr_.template cast<const void>();
        } else {
            Logger::global()->logInfo("[WebCLAP] host.get_extension('%s') is not supported.", extensionId.c_str());
        }
        return {};
    }

    void PluginInstanceWebCLAP::hostRequestRestart(void*,
                                                   wclap32::Pointer<const wclap32::wclap_host>) {
        Logger::global()->logInfo("[WebCLAP] host.request_restart()");
    }

    void PluginInstanceWebCLAP::hostRequestProcess(void*,
                                                   wclap32::Pointer<const wclap32::wclap_host>) {
        Logger::global()->logInfo("[WebCLAP] host.request_process()");
    }

    void PluginInstanceWebCLAP::hostRequestCallback(void*,
                                                    wclap32::Pointer<const wclap32::wclap_host>) {
        Logger::global()->logInfo("[WebCLAP] host.request_callback()");
    }

    void PluginInstanceWebCLAP::hostStateMarkDirty(void* ctx,
                                                   wclap32::Pointer<const wclap32::wclap_host>) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            self->stateDirty_ = true;
            Logger::global()->logInfo("[WebCLAP] host_state.mark_dirty()");
        }
    }

    void PluginInstanceWebCLAP::hostParamsRescan(void* ctx,
                                                 wclap32::Pointer<const wclap32::wclap_host>,
                                                 uint32_t flags) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_params.rescan(flags=%u)", flags);
        }
    }

    void PluginInstanceWebCLAP::hostParamsClear(void* ctx,
                                                wclap32::Pointer<const wclap32::wclap_host>,
                                                wclap32::wclap_id param,
                                                uint32_t flags) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_params.clear(param=%u, flags=%u)", param, flags);
        }
    }

    void PluginInstanceWebCLAP::hostParamsRequestFlush(void* ctx,
                                                       wclap32::Pointer<const wclap32::wclap_host>) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            self->flushRequested_ = true;
            Logger::global()->logInfo("[WebCLAP] host_params.request_flush()");
        }
    }

    bool PluginInstanceWebCLAP::hostAudioPortsIsRescanSupported(void*,
                                                                wclap32::Pointer<const wclap32::wclap_host>,
                                                                uint32_t flags) {
        constexpr uint32_t supported = wclap32::WCLAP_AUDIO_PORTS_RESCAN_NAMES |
                                       wclap32::WCLAP_AUDIO_PORTS_RESCAN_FLAGS |
                                       wclap32::WCLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT |
                                       wclap32::WCLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
                                       wclap32::WCLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR |
                                       wclap32::WCLAP_AUDIO_PORTS_RESCAN_LIST;
        return (flags & ~supported) == 0;
    }

    void PluginInstanceWebCLAP::hostAudioPortsRescan(void* ctx,
                                                     wclap32::Pointer<const wclap32::wclap_host>,
                                                     uint32_t flags) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_audio_ports.rescan(flags=%u)", flags);
        }
    }

    void PluginInstanceWebCLAP::hostGuiResizeHintsChanged(void* ctx,
                                                          wclap32::Pointer<const wclap32::wclap_host>) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_gui.resize_hints_changed()");
        }
    }

    bool PluginInstanceWebCLAP::hostGuiRequestResize(void* ctx,
                                                     wclap32::Pointer<const wclap32::wclap_host>,
                                                     uint32_t width,
                                                     uint32_t height) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_gui.request_resize(%u,%u)", width, height);
        }
        return false;
    }

    bool PluginInstanceWebCLAP::hostGuiRequestShow(void* ctx,
                                                   wclap32::Pointer<const wclap32::wclap_host>) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_gui.request_show()");
        }
        return false;
    }

    bool PluginInstanceWebCLAP::hostGuiRequestHide(void* ctx,
                                                   wclap32::Pointer<const wclap32::wclap_host>) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_gui.request_hide()");
        }
        return true;
    }

    void PluginInstanceWebCLAP::hostGuiClosed(void* ctx,
                                              wclap32::Pointer<const wclap32::wclap_host>,
                                              bool wasDestroyed) {
        if (auto* self = static_cast<PluginInstanceWebCLAP*>(ctx)) {
            Logger::global()->logInfo("[WebCLAP] host_gui.closed(destroyed=%s)", wasDestroyed ? "true" : "false");
        }
    }

    bool PluginInstanceWebCLAP::allocateAudioPort(RemoteAudioPort& port, uint32_t channels, uint32_t frames) {
        port = {};
        port.channelCount = channels;
        port.capacityFrames = frames;
        if (channels == 0 || frames == 0 || !instance_ || instance_->is64()) {
            return true;
        }
        port.channelPtrs.resize(channels);
        auto samplesPerChannel = static_cast<size_t>(frames);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            auto remote = instance_->malloc32(static_cast<uint32_t>(samplesPerChannel * sizeof(float))).template cast<float>();
            if (!remote) {
                Logger::global()->logError("[WebCLAP] Failed to allocate remote audio buffer.");
                return false;
            }
            port.channelPtrs[ch] = remote;
        }
        auto dataArray =
            instance_->malloc32(static_cast<uint32_t>(channels * sizeof(uint32_t))).template cast<wclap32::Pointer<float>>();
        if (!dataArray) {
            Logger::global()->logError("[WebCLAP] Failed to allocate remote channel pointer array.");
            return false;
        }
        instance_->setArray(dataArray, port.channelPtrs.data(), channels);
        port.data32 = dataArray;
        port.bufferCopy.data32 = dataArray;
        port.bufferCopy.data64 = {};
        port.bufferCopy.channel_count = channels;
        port.bufferCopy.latency = 0;
        port.bufferCopy.constant_mask = 0;
        auto bufferPtr =
            instance_->malloc32(static_cast<uint32_t>(sizeof(wclap32::wclap_audio_buffer))).template cast<wclap32::wclap_audio_buffer>();
        if (!bufferPtr) {
            Logger::global()->logError("[WebCLAP] Failed to allocate remote audio buffer struct.");
            return false;
        }
        instance_->set(bufferPtr, port.bufferCopy);
        port.bufferPtr = bufferPtr;
        return true;
    }

    bool PluginInstanceWebCLAP::ensureProcessStorage(uint32_t mainInputs, uint32_t mainOutputs, uint32_t frames) {
        if (!instance_ || instance_->is64())
            return false;
        if (frames == 0)
            frames = 1;
        const bool needsReallocate = processBufferCapacity_ != frames ||
                                     inputPort_.channelCount != mainInputs ||
                                     outputPort_.channelCount != mainOutputs ||
                                     !processPtr_;
        if (!needsReallocate)
            return true;
        if (!allocateAudioPort(inputPort_, mainInputs, frames))
            return false;
        if (!allocateAudioPort(outputPort_, mainOutputs, frames))
            return false;
        processPtr_ =
            instance_->malloc32(static_cast<uint32_t>(sizeof(wclap32::wclap_process))).template cast<wclap32::wclap_process>();
        if (!processPtr_) {
            Logger::global()->logError("[WebCLAP] Failed to allocate remote process struct.");
            return false;
        }
        processCopy_ = {};
        if (inputPort_.bufferPtr) {
            processCopy_.audio_inputs = inputPort_.bufferPtr.template cast<const wclap32::wclap_audio_buffer>();
            processCopy_.audio_inputs_count = inputPort_.channelCount > 0 ? 1u : 0u;
        }
        if (outputPort_.bufferPtr) {
            processCopy_.audio_outputs = outputPort_.bufferPtr;
            processCopy_.audio_outputs_count = outputPort_.channelCount > 0 ? 1u : 0u;
        }
        processCopy_.transport = {};
        processCopy_.in_events = {};
        processCopy_.out_events = {};
        instance_->set(processPtr_, processCopy_);
        processBufferCapacity_ = frames;
        tempInput_.assign(frames, 0.0f);
        tempOutput_.assign(frames, 0.0f);
        return true;
    }

    bool PluginInstanceWebCLAP::copyInputsToRemote(AudioProcessContext& context, uint32_t frames) {
        if (inputPort_.channelCount == 0)
            return true;
        const auto frameCount = static_cast<size_t>(frames);
        const bool useDouble = context.masterContext().audioDataType() == AudioContentType::Float64;
        for (uint32_t ch = 0; ch < inputPort_.channelCount; ++ch) {
            const float* source = nullptr;
            if (!useDouble) {
                source = context.getFloatInBuffer(0, ch);
            } else {
                auto* srcDouble = context.getDoubleInBuffer(0, ch);
                if (srcDouble) {
                    tempInput_.resize(frameCount);
                    for (size_t i = 0; i < frameCount; ++i)
                        tempInput_[i] = static_cast<float>(srcDouble[i]);
                    source = tempInput_.data();
                }
            }
            if (!source) {
                tempInput_.resize(frameCount);
                std::fill(tempInput_.begin(), tempInput_.end(), 0.0f);
                source = tempInput_.data();
            }
            instance_->setArray(inputPort_.channelPtrs[ch], source, frameCount);
        }
        return true;
    }

    bool PluginInstanceWebCLAP::copyOutputsFromRemote(AudioProcessContext& context, uint32_t frames) {
        if (outputPort_.channelCount == 0)
            return true;
        const auto frameCount = static_cast<size_t>(frames);
        const bool useDouble = context.masterContext().audioDataType() == AudioContentType::Float64;
        for (uint32_t ch = 0; ch < outputPort_.channelCount; ++ch) {
            float* floatDst = useDouble ? nullptr : context.getFloatOutBuffer(0, ch);
            double* doubleDst = useDouble ? context.getDoubleOutBuffer(0, ch) : nullptr;
            if (!floatDst && !doubleDst) {
                tempOutput_.resize(frameCount);
                floatDst = tempOutput_.data();
            }
            if (!floatDst && doubleDst) {
                tempOutput_.resize(frameCount);
                floatDst = tempOutput_.data();
            }
            if (!floatDst)
                continue;
            instance_->getArray(outputPort_.channelPtrs[ch], floatDst, frameCount);
            if (doubleDst) {
                for (size_t i = 0; i < frameCount; ++i)
                    doubleDst[i] = static_cast<double>(floatDst[i]);
            }
        }
        return true;
    }

    extern "C" EMSCRIPTEN_KEEPALIVE int32_t uapmd_webclap_native_process(uint32_t instanceHandle,
                                                                         uint32_t frameCount) {
        if (g_jsBridgeInstance == nullptr || g_jsBridgeContext == nullptr)
            return 0;
        if (instanceHandle != g_jsBridgeHandle)
            return 0;
        auto frames = static_cast<int32_t>(frameCount);
        frames = std::max(frames, 0);
        if (!g_jsBridgeInstance->processFromBridge(*g_jsBridgeContext, frames))
            return 0;
        return 1;
    }

}
#endif
