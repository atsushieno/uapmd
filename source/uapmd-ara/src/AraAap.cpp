#include "AraFormatBinding.hpp"
#include "AraHostDocumentController.hpp"

#include <aap/ara/araaap.hpp>
#include <aap/ara/remote-host.h>
#include <aap/core/aapxs/ara-aapxs.h>
#include <android/log.h>
#include <algorithm>
#include <cstring>
#include <exception>

namespace uapmd::ara {
    namespace {
        class AapFormatBinding final : public AraFormatBinding {
            aap::RemotePluginInstance& instance;
            aap::xs::AraClientAAPXS client;
            // The controller is destroyed before its binding. Keep host callbacks
            // installed until the SDK adapter has finished its own shutdown.
            std::unique_ptr<aap::ara::ScopedRemotePluginInstanceARAHostContext> host_context;
            std::unique_ptr<aap::ara::Host> sdk_host;
            const ARA::ARAPlugInExtensionInstance* plugin_extension{};

            std::optional<bool> importArchive(AraHostDocumentController::ArchiveScope scope,
                const std::string& archiveId, const std::vector<uint8_t>& archive,
                const std::vector<AraHostDocumentController::ArchiveTarget>& targets) {
                if (archiveId != "uapmd-aap-ara-modifications-v1")
                    return std::nullopt;
                // Decode the former host container, then let the actual SDK
                // plug-in reader validate/import each opaque modification state.
                std::vector<std::pair<ARA::ARAAudioModificationRef, std::vector<uint8_t>>> entries;
                size_t offset = 0;
                auto read = [&](std::vector<uint8_t>& bytes) {
                    uint32_t size;
                    if (archive.size() - offset < sizeof(size))
                        return false;
                    std::memcpy(&size, archive.data() + offset, sizeof(size));
                    offset += sizeof(size);
                    if (size > archive.size() - offset)
                        return false;
                    bytes.assign(archive.begin() + offset, archive.begin() + offset + size);
                    offset += size;
                    return true;
                };
                if (scope == AraHostDocumentController::ArchiveScope::Clip) {
                    if (targets.size() != 1)
                        return false;
                    entries.emplace_back(targets.front().second, archive);
                } else if (scope == AraHostDocumentController::ArchiveScope::Track) {
                    for (const auto& [_, ref] : targets) {
                        std::vector<uint8_t> bytes;
                        if (!read(bytes))
                            return false;
                        entries.emplace_back(ref, std::move(bytes));
                    }
                    if (offset != archive.size())
                        return false;
                } else {
                    while (offset < archive.size()) {
                        std::vector<uint8_t> key, bytes;
                        if (!read(key) || key.empty() || key.size() > 65536 || !read(bytes))
                            return false;
                        const std::string clipId(key.begin(), key.end());
                        auto target = std::find_if(targets.begin(), targets.end(),
                            [&](const auto& value) { return value.first == clipId; });
                        if (target != targets.end())
                            entries.emplace_back(target->second, std::move(bytes));
                    }
                }
                for (const auto& [ref, bytes] : entries)
                    if (!sdk_host->restoreModification(ref, bytes.data(), bytes.size()))
                        return false;
                return true;
            }

        public:
            AapFormatBinding(aap::RemotePluginInstance& instance, AAPXSInitiatorInstance* initiator)
                : instance(instance), client(initiator, initiator->serialization) {}

            std::string_view formatName() const override { return "AAP"; }
            const ARA::ARAFactory* factory() const override {
                return sdk_host ? sdk_host->factory() : nullptr;
            }

            std::unique_ptr<AraDocumentController> createDocumentController(std::string name) override {
                if (sdk_host)
                    return nullptr;
                try {
                    auto controller = std::make_unique<AraHostDocumentController>(
                        [this](const ARA::ARADocumentControllerHostInstance* services,
                               const ARA::ARADocumentProperties* properties) {
                            sdk_host = std::make_unique<aap::ara::Host>(instance.getPlugin(),
                                client.asPluginExtension(), services, properties);
                            host_context = std::make_unique<aap::ara::ScopedRemotePluginInstanceARAHostContext>(
                                sdk_host->transportExtension(), instance);
                            __android_log_print(ANDROID_LOG_INFO, "uapmd-ara", "ARAAAP controller created for %d", instance.getInstanceId());
                            return sdk_host->documentController();
                        }, std::move(name));
                    controller->setArchiveImporter([this](auto scope, const auto& archiveId,
                                                         const auto& archive, const auto& targets) {
                        return importArchive(scope, archiveId, archive, targets);
                    });
                    return controller;
                } catch (const std::exception& error) {
                    __android_log_print(ANDROID_LOG_WARN, "uapmd-ara", "ARAAAP controller: %s", error.what());
                    sdk_host.reset();
                    host_context.reset();
                    return nullptr;
                }
            }

            const ARA::ARAPlugInExtensionInstance* bindToDocumentController(
                ARA::ARADocumentControllerRef documentControllerRef,
                ARA::ARAPlugInInstanceRoleFlags knownRoles,
                ARA::ARAPlugInInstanceRoleFlags assignedRoles) override {
                if (!sdk_host || documentControllerRef != sdk_host->documentController()->documentControllerRef)
                    return nullptr;
                try {
                    if (!plugin_extension)
                        plugin_extension = sdk_host->bind(knownRoles, assignedRoles);
                    __android_log_print(ANDROID_LOG_INFO, "uapmd-ara", "ARAAAP roles bound for %d", instance.getInstanceId());
                    return plugin_extension;
                } catch (const std::exception& error) {
                    __android_log_print(ANDROID_LOG_WARN, "uapmd-ara", "ARAAAP binding: %s", error.what());
                    return nullptr;
                }
            }
        };
    }

    std::unique_ptr<AraFormatBinding> createAapAraBinding(AraPluginInstanceHandleExtension& handles) {
        auto* instance = static_cast<aap::RemotePluginInstance*>(
            handles.nativeHandle(AraPluginInstanceHandleKind::AAPRemotePluginInstance));
        if (!instance) {
            __android_log_print(ANDROID_LOG_DEBUG, "uapmd-ara", "No remote AAP instance handle");
            return nullptr;
        }
        // Host-registered dispatchers alone do not establish plug-in support.
        // Require the SDK transport version advertised by this plug-in binary.
        auto* information = instance->getPluginInformation();
        if (!information || !information->hasExtension(ARA_AAP_TRANSPORT_EXTENSION_URI) || !instance->getPlugin()) {
            __android_log_print(ANDROID_LOG_DEBUG, "uapmd-ara", "AAP instance %d does not advertise %s",
                instance->getInstanceId(), ARA_AAP_TRANSPORT_EXTENSION_URI);
            return nullptr;
        }
        auto* initiator = instance->getAAPXSDispatcher().getPluginAAPXSByUri(ARA_AAP_TRANSPORT_EXTENSION_URI);
        if (!initiator || !initiator->serialization) {
            __android_log_print(ANDROID_LOG_WARN, "uapmd-ara", "AAP instance %d has no ARAAAP dispatcher",
                instance->getInstanceId());
            return nullptr;
        }
        return std::make_unique<AapFormatBinding>(*instance, initiator);
    }
}
