#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include <ARA_API/ARAInterface.h>
#include <uapmd-plugin-hosting/uapmd-plugin-hosting.hpp>
#include <uapmd-ara/ara-plugin-instance-handles.hpp>

namespace uapmd::ara {

    class AraFormatBinding {
    public:
        virtual ~AraFormatBinding() = default;

        virtual std::string_view formatName() const = 0;
        virtual const ARA::ARAFactory* factory() const = 0;

        virtual const ARA::ARAPlugInExtensionInstance* bindToDocumentController(
            ARA::ARADocumentControllerRef documentControllerRef,
            ARA::ARAPlugInInstanceRoleFlags knownRoles,
            ARA::ARAPlugInInstanceRoleFlags assignedRoles) = 0;
    };

    std::unique_ptr<AraFormatBinding> createVst3AraBinding(AraPluginInstanceHandleExtension& araHandles);
    std::unique_ptr<AraFormatBinding> createClapAraBinding(AraPluginInstanceHandleExtension& araHandles);
    std::unique_ptr<AraFormatBinding> createAudioUnitAraBinding(AraPluginInstanceHandleExtension& araHandles);
    std::unique_ptr<AraFormatBinding> createAraFormatBinding(AudioPluginInstanceAPI& pluginInstance);

} // namespace uapmd::ara
