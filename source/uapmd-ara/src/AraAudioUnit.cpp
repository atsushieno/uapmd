#include "AraFormatBinding.hpp"

#if __APPLE__
#include <ARA_API/ARAAudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace uapmd::ara {

#if __APPLE__
    namespace {
        class AudioUnitAraBinding final : public AraFormatBinding {
            AudioUnit audio_unit_{};
            const ARA::ARAFactory* factory_{};

        public:
            explicit AudioUnitAraBinding(AudioUnit audioUnit)
                : audio_unit_(audioUnit) {
                if (!audio_unit_)
                    return;

                ARA::ARAAudioUnitFactory factoryProperty{
                    .inOutMagicNumber = ARA::kARAAudioUnitMagic,
                    .outFactory = nullptr
                };
                UInt32 size = sizeof(factoryProperty);
                auto status = AudioUnitGetProperty(
                    audio_unit_,
                    ARA::kAudioUnitProperty_ARAFactory,
                    kAudioUnitScope_Global,
                    0,
                    &factoryProperty,
                    &size);
                if (status == noErr &&
                    size == sizeof(factoryProperty) &&
                    factoryProperty.inOutMagicNumber == ARA::kARAAudioUnitMagic)
                    factory_ = factoryProperty.outFactory;
            }

            bool usable() const {
                return audio_unit_ && factory_;
            }

            std::string_view formatName() const override {
                return "AudioUnit";
            }

            const ARA::ARAFactory* factory() const override {
                return factory_;
            }

            const ARA::ARAPlugInExtensionInstance* bindToDocumentController(
                ARA::ARADocumentControllerRef documentControllerRef,
                ARA::ARAPlugInInstanceRoleFlags knownRoles,
                ARA::ARAPlugInInstanceRoleFlags assignedRoles) override {
                if (!documentControllerRef || !usable())
                    return nullptr;

                ARA::ARAAudioUnitPlugInExtensionBinding bindingProperty{
                    .inOutMagicNumber = ARA::kARAAudioUnitMagic,
                    .inDocumentControllerRef = documentControllerRef,
                    .outPlugInExtension = nullptr,
                    .knownRoles = knownRoles,
                    .assignedRoles = assignedRoles
                };
                UInt32 size = sizeof(bindingProperty);
                auto status = AudioUnitGetProperty(
                    audio_unit_,
                    ARA::kAudioUnitProperty_ARAPlugInExtensionBindingWithRoles,
                    kAudioUnitScope_Global,
                    0,
                    &bindingProperty,
                    &size);
                if (status != noErr ||
                    size != sizeof(bindingProperty) ||
                    bindingProperty.inOutMagicNumber != ARA::kARAAudioUnitMagic)
                    return nullptr;
                return bindingProperty.outPlugInExtension;
            }
        };
    } // namespace
#endif

    std::unique_ptr<AraFormatBinding> createAudioUnitAraBinding(AraPluginInstanceHandleExtension& araHandles) {
#if __APPLE__
        auto* audioUnit = static_cast<AudioUnit>(
            araHandles.nativeHandle(AraPluginInstanceHandleKind::AudioUnitV2));
        if (!audioUnit)
            audioUnit = static_cast<AudioUnit>(
                araHandles.nativeHandle(AraPluginInstanceHandleKind::AudioUnitV3BridgedV2));

        auto binding = std::make_unique<AudioUnitAraBinding>(audioUnit);
        if (!binding->usable())
            return nullptr;
        return binding;
#else
        (void) native;
        return nullptr;
#endif
    }

} // namespace uapmd::ara
