#include "AraFormatBinding.hpp"

#include <ARA_API/ARAVST3.h>

namespace ARA {
    DEF_CLASS_IID(IMainFactory)
    DEF_CLASS_IID(IPlugInEntryPoint)
    DEF_CLASS_IID(IPlugInEntryPoint2)
}

namespace uapmd::ara {

    namespace {
        class Vst3AraBinding final : public AraFormatBinding {
            ARA::IPlugInEntryPoint* entry_point_{};
            ARA::IPlugInEntryPoint2* entry_point2_{};
            const ARA::ARAFactory* factory_{};

        public:
            explicit Vst3AraBinding(Steinberg::FUnknown* unknown) {
                if (!unknown)
                    return;

                if (unknown->queryInterface(ARA::IPlugInEntryPoint::iid, reinterpret_cast<void**>(&entry_point_)) != Steinberg::kResultOk)
                    entry_point_ = nullptr;
                if (unknown->queryInterface(ARA::IPlugInEntryPoint2::iid, reinterpret_cast<void**>(&entry_point2_)) != Steinberg::kResultOk)
                    entry_point2_ = nullptr;

                if (entry_point_)
                    factory_ = entry_point_->getFactory();
            }

            ~Vst3AraBinding() override {
                if (entry_point2_)
                    entry_point2_->release();
                if (entry_point_)
                    entry_point_->release();
            }

            bool usable() const {
                return factory_ && (entry_point_ || entry_point2_);
            }

            std::string_view formatName() const override {
                return "VST3";
            }

            const ARA::ARAFactory* factory() const override {
                return factory_;
            }

            const ARA::ARAPlugInExtensionInstance* bindToDocumentController(
                ARA::ARADocumentControllerRef documentControllerRef,
                ARA::ARAPlugInInstanceRoleFlags knownRoles,
                ARA::ARAPlugInInstanceRoleFlags assignedRoles) override {
                if (!documentControllerRef)
                    return nullptr;
                if (entry_point2_)
                    return entry_point2_->bindToDocumentControllerWithRoles(documentControllerRef, knownRoles, assignedRoles);
                if (entry_point_)
                    return entry_point_->bindToDocumentController(documentControllerRef);
                return nullptr;
            }
        };

        std::unique_ptr<AraFormatBinding> createBindingFromHandle(void* handle) {
            auto binding = std::make_unique<Vst3AraBinding>(static_cast<Steinberg::FUnknown*>(handle));
            if (!binding->usable())
                return nullptr;
            return binding;
        }
    } // namespace

    std::unique_ptr<AraFormatBinding> createVst3AraBinding(AraPluginInstanceHandleExtension& araHandles) {
        if (auto binding = createBindingFromHandle(
                araHandles.nativeHandle(AraPluginInstanceHandleKind::VST3Component)))
            return binding;
        return createBindingFromHandle(
            araHandles.nativeHandle(AraPluginInstanceHandleKind::VST3Unknown));
    }

} // namespace uapmd::ara
