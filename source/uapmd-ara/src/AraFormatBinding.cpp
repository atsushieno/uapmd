#include "AraFormatBinding.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Which bindings exist is decided in uapmd-ara's CMakeLists, which builds the VST3 and
// CLAP ones only where remidy hosts those formats. This has to agree with it.
#if defined(ANDROID) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define UAPMD_ARA_HAS_VST3_AND_CLAP 0
#else
#define UAPMD_ARA_HAS_VST3_AND_CLAP 1
#endif

using namespace uapmd_plugin_hosting;

namespace uapmd::ara {

    namespace {
        AraPluginInstanceHandleExtension* araHandleExtension(AudioPluginInstanceAPI& pluginInstance) {
            auto* extension = pluginInstance.extension(kAraPluginInstanceHandleExtensionId);
            return dynamic_cast<AraPluginInstanceHandleExtension*>(extension);
        }
    }

    std::unique_ptr<AraFormatBinding> createAraFormatBinding(AudioPluginInstanceAPI& pluginInstance) {
        auto* araHandles = araHandleExtension(pluginInstance);
        if (!araHandles)
            return nullptr;
#if ANDROID
        // FIXME: implement
        //if (auto binding = createAapAraBinding(*araHandles))
        //    return binding;
#endif
#if UAPMD_ARA_HAS_VST3_AND_CLAP
        if (auto binding = createVst3AraBinding(*araHandles))
            return binding;
        if (auto binding = createClapAraBinding(*araHandles))
            return binding;
#endif
#if defined(__APPLE__)
        if (auto binding = createAudioUnitAraBinding(*araHandles))
            return binding;
#endif
        return nullptr;
    }

} // namespace uapmd::ara
