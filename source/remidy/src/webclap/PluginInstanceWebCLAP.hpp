#pragma once

#include "remidy/priv/plugin-format-webclap.hpp"

#if defined(__EMSCRIPTEN__)

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <wclap/wclap.hpp>
#include "wclap-js-instance.h"

namespace remidy {

    using WebclapSdkInstance = ::Instance;

    class PluginInstanceWebCLAP : public PluginInstance {
    public:
        PluginInstanceWebCLAP(PluginCatalogEntry* entry,
                              std::unique_ptr<WebclapSdkInstance> sdkInstance,
                              std::string cacheRoot);
        ~PluginInstanceWebCLAP() override;

        PluginUIThreadRequirement requiresUIThreadOn() override;
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;
        StatusCode process(AudioProcessContext& context) override;

        uint32_t webHandle() const;
        bool processFromBridge(AudioProcessContext& context, int32_t frames);

        PluginAudioBuses* audioBuses() override;
        PluginParameterSupport* parameters() override;
        PluginStateSupport* states() override;
        PluginPresetsSupport* presets() override;
        PluginUISupport* ui() override;
        bool requiresReplacingProcess() const override;

        const std::string& cacheRoot() const;

    private:
        struct RemoteAudioPort {
            uint32_t channelCount{0};
            uint32_t capacityFrames{0};
            wclap32::Pointer<wclap32::wclap_audio_buffer> bufferPtr{};
            wclap32::wclap_audio_buffer bufferCopy{};
            wclap32::Pointer<wclap32::Pointer<float>> data32{};
            std::vector<wclap32::Pointer<float>> channelPtrs{};
        };

        uint32_t handleValue() const;
        bool invokeConfigure(uint32_t sampleRate,
                             uint32_t bufferSize,
                             uint32_t mainInputs,
                             uint32_t mainOutputs);
        bool invokeStart();
        bool invokeStop();
        bool invokeProcess(AudioProcessContext& context, int32_t frameCount);

        static wclap32::Pointer<const void> hostGetExtension(void* ctx,
                                                             wclap32::Pointer<const wclap32::wclap_host> host,
                                                             wclap32::Pointer<const char> id);
        static void hostRequestRestart(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static void hostRequestProcess(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static void hostRequestCallback(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static void hostStateMarkDirty(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static void hostParamsRescan(void* ctx,
                                     wclap32::Pointer<const wclap32::wclap_host> host,
                                     uint32_t flags);
        static void hostParamsClear(void* ctx,
                                    wclap32::Pointer<const wclap32::wclap_host> host,
                                    wclap32::wclap_id param,
                                    uint32_t flags);
        static void hostParamsRequestFlush(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static bool hostAudioPortsIsRescanSupported(void* ctx,
                                                    wclap32::Pointer<const wclap32::wclap_host> host,
                                                    uint32_t flags);
        static void hostAudioPortsRescan(void* ctx,
                                         wclap32::Pointer<const wclap32::wclap_host> host,
                                         uint32_t flags);
        static void hostGuiResizeHintsChanged(void* ctx,
                                              wclap32::Pointer<const wclap32::wclap_host> host);
        static bool hostGuiRequestResize(void* ctx,
                                         wclap32::Pointer<const wclap32::wclap_host> host,
                                         uint32_t width,
                                         uint32_t height);
        static bool hostGuiRequestShow(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static bool hostGuiRequestHide(void* ctx, wclap32::Pointer<const wclap32::wclap_host> host);
        static void hostGuiClosed(void* ctx,
                                  wclap32::Pointer<const wclap32::wclap_host> host,
                                  bool wasDestroyed);

        bool ensureEntryInitialized();
        bool ensureFactory();
        bool ensureHostStruct();
        bool ensureHostState();
        bool ensureHostParams();
        bool ensureHostAudioPorts();
        bool ensureHostGui();
        bool allocateAudioPort(RemoteAudioPort& port, uint32_t channels, uint32_t frames);
        bool ensureProcessStorage(uint32_t mainInputs, uint32_t mainOutputs, uint32_t frames);
        bool copyInputsToRemote(AudioProcessContext& context, uint32_t frames);
        bool copyOutputsFromRemote(AudioProcessContext& context, uint32_t frames);
        bool ensurePlugin();
        bool ensureProcessStruct();
        void destroyPlugin();

        wclap32::Pointer<char> writeString(const std::string& value);
        std::string readString(wclap32::Pointer<const char> value);

        std::unique_ptr<WebclapSdkInstance> instance_;
        std::string cacheRoot_;
        ConfigurationRequest lastConfiguration_{};
        bool configured_{false};
        bool running_{false};
        bool entryInitialized_{false};
        bool sdkInitialized_{false};
        bool pluginInitialized_{false};
        bool stateDirty_{false};
        bool flushRequested_{false};

        wclap32::wclap_plugin_entry entryCopy_{};
        wclap32::wclap_plugin_factory factoryCopy_{};
        wclap32::Pointer<const wclap32::wclap_plugin_factory> factoryPtr_{};
        wclap32::Pointer<const wclap32::wclap_plugin_descriptor> descriptorPtr_{};
        wclap32::Pointer<const wclap32::wclap_plugin> pluginPtr_{};
        wclap32::Pointer<wclap32::wclap_host> hostPtr_{};
        wclap32::Pointer<char> pluginPathPtr_{};

        wclap32::wclap_plugin plugin_{};
        wclap32::wclap_host hostStruct_{};
        std::vector<wclap32::Pointer<char>> pinnedStrings_{};
        struct HostExtensions {
            wclap32::wclap_host_state state{};
            wclap32::wclap_host_params params{};
            wclap32::wclap_host_audio_ports audioPorts{};
            wclap32::wclap_host_gui gui{};
            wclap32::Pointer<wclap32::wclap_host_state> statePtr_;
            wclap32::Pointer<wclap32::wclap_host_params> paramsPtr_;
            wclap32::Pointer<wclap32::wclap_host_audio_ports> audioPortsPtr_;
            wclap32::Pointer<wclap32::wclap_host_gui> guiPtr_;
        } hostExtensions_{};
        uint32_t processBufferCapacity_{0};
        RemoteAudioPort inputPort_{};
        RemoteAudioPort outputPort_{};
        wclap32::Pointer<wclap32::wclap_process> processPtr_{};
        wclap32::wclap_process processCopy_{};
        std::vector<float> tempInput_;
        std::vector<float> tempOutput_;
    };

}

#endif
