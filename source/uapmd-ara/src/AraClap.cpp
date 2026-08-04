#include "AraFormatBinding.hpp"

#include <ARA_API/ARACLAP.h>

namespace uapmd::ara {

    namespace {
        class ClapAraBinding final : public AraFormatBinding {
            clap_plugin_t* plugin_{};
            const clap_ara_plugin_extension_t* extension_{};
            const ARA::ARAFactory* factory_{};

        public:
            explicit ClapAraBinding(clap_plugin_t* plugin)
                : plugin_(plugin) {
                if (!plugin_ || !plugin_->get_extension)
                    return;

                extension_ = static_cast<const clap_ara_plugin_extension_t*>(
                    plugin_->get_extension(plugin_, CLAP_EXT_ARA_PLUGINEXTENSION));
                if (extension_ && extension_->get_factory)
                    factory_ = extension_->get_factory(plugin_);
            }

            bool usable() const {
                return plugin_ && extension_ && extension_->bind_to_document_controller && factory_;
            }

            std::string_view formatName() const override {
                return "CLAP";
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
                return extension_->bind_to_document_controller(plugin_, documentControllerRef, knownRoles, assignedRoles);
            }
        };
    } // namespace

    std::unique_ptr<AraFormatBinding> createClapAraBinding(AraPluginInstanceHandleExtension& araHandles) {
        auto* plugin = static_cast<clap_plugin_t*>(
            araHandles.nativeHandle(AraPluginInstanceHandleKind::CLAPPlugin));
        auto binding = std::make_unique<ClapAraBinding>(plugin);
        if (!binding->usable())
            return nullptr;
        return binding;
    }

} // namespace uapmd::ara
