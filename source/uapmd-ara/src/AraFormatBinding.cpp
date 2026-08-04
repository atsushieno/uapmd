#include "AraFormatBinding.hpp"

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
#else
        if (auto binding = createVst3AraBinding(*araHandles))
            return binding;
        if (auto binding = createClapAraBinding(*araHandles))
            return binding;
#if defined(__APPLE__)
        if (auto binding = createAudioUnitAraBinding(*araHandles))
            return binding;
#endif
#endif
        return nullptr;
    }

} // namespace uapmd::ara
