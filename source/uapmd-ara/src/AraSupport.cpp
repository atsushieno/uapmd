#include "uapmd-ara/uapmd-ara.hpp"

#include "AraFormatBinding.hpp"
#include "AraHostDocumentController.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace uapmd_plugin_hosting;

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

            // ARA archives are stored against a plugin's persistent graph node
            // identity rather than its runtime instance id, which is allocated
            // per session and does not survive a reload. Keying by the instance
            // id bound a plugin's archived state to whichever plugin happened
            // to be allocated that id next time.
            std::string nodeIdForInstance(int32_t instanceId) {
                for (auto* track : engine_.tracks())
                    if (track)
                        if (auto* node = track->graph().getPluginNode(instanceId))
                            return node->nodeId();
                if (auto* master = engine_.masterTrack())
                    if (auto* node = master->graph().getPluginNode(instanceId))
                        return node->nodeId();
                return {};
            }

            int32_t instanceIdForNodeId(const std::string& nodeId) {
                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    (void) document;
                    if (nodeIdForInstance(pluginInstanceId) == nodeId)
                        return pluginInstanceId;
                }
                return -1;
            }

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
                // v2 keys entries by persistent graph node identity. v1 keyed
                // them by runtime instance id and is still read, below.
                manifest << "uapmd-ara-state-v2\n";

                size_t archiveIndex = 0;
                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    if (!document.controller)
                        continue;

                    // Node ids may contain characters that are awkward in a
                    // filename, so the archive keeps a positional name and the
                    // manifest carries the identity.
                    const auto nodeId = nodeIdForInstance(pluginInstanceId);
                    if (nodeId.empty()) {
                        std::cerr << "Warning: No graph node found for plugin instance " << pluginInstanceId
                                  << "; its ARA state will not be archived." << std::endl;
                        continue;
                    }

                    std::vector<uint8_t> archive;
                    if (!document.controller->saveArchiveState(archive)) {
                        error = std::format("Failed to archive native ARA document for plugin instance {}.", pluginInstanceId);
                        return false;
                    }
                    if (archive.empty())
                        continue;

                    const auto archivePath = std::filesystem::path("native") / (std::to_string(archiveIndex++) + ".bin");
                    if (!context.writeExtensionFile(extensionId(), archivePath, archive, error))
                        return false;
                    // Node id last, so that it may contain spaces.
                    manifest << "native " << archivePath.generic_string() << " " << nodeId << "\n";
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
                // v1 entries are "native <instanceId> <path>", keyed by a
                // runtime instance id that is meaningless in this session.
                // Those entries are read positionally and may bind to the wrong
                // plugin; nothing better is recoverable from them.
                const bool keyedByNodeId = header == "uapmd-ara-state-v2";
                if (!keyedByNodeId && header != "uapmd-ara-state-v1") {
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
                    if (keyedByNodeId) {
                        std::string nodeId;
                        if (!(manifest >> archivePath) || !std::getline(manifest, nodeId)) {
                            error = "Malformed ARA extension manifest entry.";
                            return false;
                        }
                        // Node id runs to end of line, so trim the separator.
                        const auto firstNonSpace = nodeId.find_first_not_of(" \t\r");
                        nodeId = firstNonSpace == std::string::npos ? std::string{} : nodeId.substr(firstNonSpace);
                        while (!nodeId.empty() && (nodeId.back() == '\r' || nodeId.back() == ' '))
                            nodeId.pop_back();
                        pluginInstanceId = instanceIdForNodeId(nodeId);
                    } else if (!(manifest >> pluginInstanceId >> archivePath)) {
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

            // Hold one ARA edit cycle open for the whole batch, so that a
            // multi-step edit is applied atomically rather than as one cycle
            // per event.
            void transactionBegan() override {
                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    (void) pluginInstanceId;
                    if (document.controller)
                        document.controller->beginProjectDocumentTransaction();
                }
            }

            void transactionEnded() override {
                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    (void) pluginInstanceId;
                    if (document.controller)
                        document.controller->endProjectDocumentTransaction();
                }
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
