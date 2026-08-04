#include "uapmd-ara/uapmd-ara.hpp"

#include "AraFormatBinding.hpp"
#include "AraHostDocumentController.hpp"

#include <filesystem>
#include <format>
#include <map>
#include <sstream>
#include <stdexcept>

namespace uapmd::ara {

    namespace {
        constexpr std::string_view kAraProjectSerializationExtensionId =
            "dev.atsushieno.uapmd.ara.state.v1";

        std::vector<uint8_t> bytesFromString(const std::string& value) {
            return std::vector<uint8_t>(value.begin(), value.end());
        }

        std::string stringFromBytes(const std::vector<uint8_t>& bytes) {
            return std::string(bytes.begin(), bytes.end());
        }

        class AraSupportImpl final
            : public AraSupport
            , public ProjectDocumentEventListener
            , public ProjectSerializationExtension {
            struct NativeAraDocument {
                std::unique_ptr<AraFormatBinding> binding{};
                std::unique_ptr<AraHostDocumentController> controller{};
                const ARA::ARAPlugInExtensionInstance* plugin_extension{};
            };

            SequencerEngine& engine_;
            std::unique_ptr<AraSession> session_;
            ProjectDocumentEventSource* event_source_{};
            ProjectDocumentEventListenerToken event_listener_token_{};
            std::map<int32_t, NativeAraDocument> native_ara_documents_{};
            std::map<int32_t, std::vector<uint8_t>> pending_native_archives_{};

            void resyncNativeAraDocuments() {
                auto masterTrackSnapshot = engine_.timeline().buildMasterTrackSnapshot();
                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    (void) pluginInstanceId;
                    if (document.controller)
                        document.controller->resyncFromProjectDocument(
                            engine_.timeline().projectDocumentView(),
                            masterTrackSnapshot);
                }
            }

            void applyNativeAraProjectEvent(const ProjectDocumentEvent& event) {
                auto masterTrackSnapshot = engine_.timeline().buildMasterTrackSnapshot();
                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    (void) pluginInstanceId;
                    if (document.controller)
                        document.controller->applyProjectDocumentEvent(
                            engine_.timeline().projectDocumentView(),
                            masterTrackSnapshot,
                            event);
                }
            }

        public:
            explicit AraSupportImpl(SequencerEngine& engine)
                : engine_(engine)
                , session_(AraSession::create()) {
                if (!session_)
                    throw std::runtime_error("Failed to create ARA session.");

                auto status = session_->bindProjectDocument(
                    engine_.timeline().projectDocumentView(),
                    engine_.timeline().projectDocumentEvents());
                if (status != AraStatus::Ok)
                    throw std::runtime_error("Failed to bind ARA session to project document.");

                engine_.timeline().addProjectSerializationExtension(*this);
                event_source_ = &engine_.timeline().projectDocumentEvents();
                event_listener_token_ = event_source_->addProjectDocumentEventListener(*this);
            }

            ~AraSupportImpl() override {
                if (event_source_ && event_listener_token_ != 0)
                    event_source_->removeProjectDocumentEventListener(event_listener_token_);
                engine_.timeline().removeProjectSerializationExtension(*this);
                if (session_)
                    session_->unbindProjectDocument();
            }

            AraSession& session() override {
                return *session_;
            }

            const AraSession& session() const override {
                return *session_;
            }

            AraStatus attachPlugin(
                int32_t pluginInstanceId,
                AudioPluginInstanceAPI& pluginInstance) override {
                auto status = session_->attachPlugin(pluginInstanceId, pluginInstance);
                if (status != AraStatus::UnsupportedPlugin)
                    return status;

                auto nativeBinding = createAraFormatBinding(pluginInstance);
                if (!nativeBinding)
                    return AraStatus::UnsupportedPlugin;

                auto controller = std::make_unique<AraHostDocumentController>(
                    *nativeBinding->factory(),
                    "uapmd");
                if (!controller->valid())
                    return AraStatus::BackendError;

                if (!controller->resyncFromProjectDocument(
                        engine_.timeline().projectDocumentView(),
                        engine_.timeline().buildMasterTrackSnapshot()))
                    return AraStatus::BackendError;

                if (auto pendingIt = pending_native_archives_.find(pluginInstanceId); pendingIt != pending_native_archives_.end()) {
                    if (!controller->loadArchiveState(pendingIt->second))
                        return AraStatus::BackendError;
                    pending_native_archives_.erase(pendingIt);
                }

                const auto knownRoles =
                    ARA::kARAPlaybackRendererRole |
                    ARA::kARAEditorRendererRole |
                    ARA::kARAEditorViewRole;
                auto* pluginExtension = nativeBinding->bindToDocumentController(
                    controller->documentControllerRef(),
                    knownRoles,
                    knownRoles);
                if (!pluginExtension)
                    return AraStatus::BackendError;
                controller->bindPluginExtension(pluginExtension);

                native_ara_documents_.emplace(
                    pluginInstanceId,
                    NativeAraDocument{
                        .binding = std::move(nativeBinding),
                        .controller = std::move(controller),
                        .plugin_extension = pluginExtension
                    });
                return AraStatus::Ok;
            }

            void detachPlugin(int32_t pluginInstanceId) override {
                if (auto it = native_ara_documents_.find(pluginInstanceId); it != native_ara_documents_.end()) {
                    if (it->second.controller)
                        it->second.controller->bindPluginExtension(nullptr);
                    native_ara_documents_.erase(it);
                }
                session_->detachPlugin(pluginInstanceId);
            }

            bool hasNativeAraBinding(int32_t pluginInstanceId) const override {
                return native_ara_documents_.contains(pluginInstanceId);
            }

            const ARA::ARAFactory* nativeAraFactory(int32_t pluginInstanceId) const override {
                auto it = native_ara_documents_.find(pluginInstanceId);
                if (it == native_ara_documents_.end())
                    return nullptr;
                return it->second.binding->factory();
            }

            const ARA::ARAPlugInExtensionInstance* bindNativeAraPlugin(
                int32_t pluginInstanceId,
                ARA::ARADocumentControllerRef documentControllerRef,
                ARA::ARAPlugInInstanceRoleFlags knownRoles,
                ARA::ARAPlugInInstanceRoleFlags assignedRoles) override {
                auto it = native_ara_documents_.find(pluginInstanceId);
                if (it == native_ara_documents_.end())
                    return nullptr;
                if (!documentControllerRef)
                    return it->second.plugin_extension;
                return it->second.binding->bindToDocumentController(documentControllerRef, knownRoles, assignedRoles);
            }

            AraRequestId requestAnalysis(
                int32_t pluginInstanceId,
                AraAnalysisRequest request,
                AraAnalysisCallback callback) override {
                auto nativeIt = native_ara_documents_.find(pluginInstanceId);
                if (nativeIt != native_ara_documents_.end() && nativeIt->second.controller)
                    return nativeIt->second.controller->requestAnalysis(
                        std::move(request),
                        std::move(callback));

                auto* document = session_->pluginDocument(pluginInstanceId);
                if (!document)
                    return 0;
                return document->requestAnalysis(std::move(request), std::move(callback));
            }

            void cancelAnalysis(int32_t pluginInstanceId, AraRequestId requestId) override {
                auto nativeIt = native_ara_documents_.find(pluginInstanceId);
                if (nativeIt != native_ara_documents_.end() && nativeIt->second.controller) {
                    nativeIt->second.controller->cancelAnalysis(requestId);
                    return;
                }

                auto* document = session_->pluginDocument(pluginInstanceId);
                if (document)
                    document->cancelAnalysis(requestId);
            }

            std::string_view extensionId() const override {
                return kAraProjectSerializationExtensionId;
            }

            bool saveProjectExtensionData(
                ProjectSerializationWriteContext& context,
                std::string& error) override {
                std::ostringstream manifest;
                manifest << "uapmd-ara-state-v1\n";

                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    if (!document.controller)
                        continue;

                    std::vector<uint8_t> archive;
                    if (!document.controller->saveArchiveState(archive)) {
                        error = std::format("Failed to archive native ARA document for plugin instance {}.", pluginInstanceId);
                        return false;
                    }
                    if (archive.empty())
                        continue;

                    const auto archivePath = std::filesystem::path("native") / (std::to_string(pluginInstanceId) + ".bin");
                    if (!context.writeExtensionFile(extensionId(), archivePath, archive, error))
                        return false;
                    manifest << "native " << pluginInstanceId << " " << archivePath.generic_string() << "\n";
                }

                return context.writeExtensionFile(
                    extensionId(),
                    "manifest.txt",
                    bytesFromString(manifest.str()),
                    error);
            }

            bool loadProjectExtensionData(
                ProjectSerializationReadContext& context,
                std::string& error) override {
                pending_native_archives_.clear();

                auto manifestBytes = context.readExtensionFile(extensionId(), "manifest.txt", error);
                if (!manifestBytes) {
                    error.clear();
                    return true;
                }

                std::istringstream manifest(stringFromBytes(*manifestBytes));
                std::string header;
                std::getline(manifest, header);
                if (header != "uapmd-ara-state-v1") {
                    error = "Unsupported ARA extension manifest.";
                    return false;
                }

                std::string kind;
                while (manifest >> kind) {
                    if (kind != "native") {
                        error = "Unsupported ARA extension manifest entry.";
                        return false;
                    }

                    int32_t pluginInstanceId{};
                    std::string archivePath;
                    if (!(manifest >> pluginInstanceId >> archivePath)) {
                        error = "Malformed ARA extension manifest entry.";
                        return false;
                    }

                    auto archiveBytes = context.readExtensionFile(extensionId(), archivePath, error);
                    if (!archiveBytes)
                        return false;

                    auto documentIt = native_ara_documents_.find(pluginInstanceId);
                    if (documentIt != native_ara_documents_.end() && documentIt->second.controller) {
                        if (!documentIt->second.controller->loadArchiveState(*archiveBytes)) {
                            error = std::format("Failed to restore native ARA document for plugin instance {}.", pluginInstanceId);
                            return false;
                        }
                    } else {
                        pending_native_archives_[pluginInstanceId] = std::move(*archiveBytes);
                    }
                }

                return true;
            }

            void projectLoaded(const ProjectDocumentEvent& event) override {
                (void) event;
                resyncNativeAraDocuments();
            }

            void projectClosing(const ProjectDocumentEvent& event) override {
                (void) event;
                resyncNativeAraDocuments();
            }

            void masterTrackChanged(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void trackAdded(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void trackRemoved(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void trackChanged(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void clipAdded(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void clipRemoved(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void clipChanged(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void audioSourceAdded(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void audioSourceRemoved(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }

            void audioSourceChanged(const ProjectDocumentEvent& event) override {
                applyNativeAraProjectEvent(event);
            }
        };
    } // namespace

    std::unique_ptr<AraSupport> createAraSupport(SequencerEngine& engine) {
        return std::make_unique<AraSupportImpl>(engine);
    }

} // namespace uapmd::ara
