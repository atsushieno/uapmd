#include "uapmd-ara/uapmd-ara.hpp"

#include "AraFormatBinding.hpp"
#include "AraHostDocumentController.hpp"

#include <cstring>
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
            // Archive bytes plus the plug-in archive identifier they were written
            // under; ARA refuses a restore that cannot name its own format.
            struct PendingArchive {
                std::string archive_id;
                std::vector<uint8_t> bytes;
            };
            std::map<int32_t, PendingArchive> pending_native_archives_{};

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

            // A clip's ARA state is not one archive: every plug-in instance
            // hosting an ARA document holds its own. The slot therefore carries
            // one entry per document, keyed by the plug-in's persistent graph
            // node identity, and each entry records the persistent IDs its
            // archive was taken from so a paste can remap them.
            //
            // Framed by hand rather than as JSON because the archives are
            // arbitrary binary and the slot is opaque to everyone but this
            // extension.
            static void appendChunk(std::vector<uint8_t>& out, const void* data, size_t size) {
                const auto length = static_cast<uint32_t>(size);
                const auto* lengthBytes = reinterpret_cast<const uint8_t*>(&length);
                out.insert(out.end(), lengthBytes, lengthBytes + sizeof(length));
                const auto* bytes = static_cast<const uint8_t*>(data);
                out.insert(out.end(), bytes, bytes + size);
            }

            static void appendChunk(std::vector<uint8_t>& out, const std::string& value) {
                appendChunk(out, value.data(), value.size());
            }

            static bool readChunk(const std::vector<uint8_t>& in, size_t& offset, std::vector<uint8_t>& out) {
                uint32_t length{};
                if (offset + sizeof(length) > in.size())
                    return false;
                std::memcpy(&length, in.data() + offset, sizeof(length));
                offset += sizeof(length);
                if (offset + length > in.size())
                    return false;
                out.assign(in.begin() + static_cast<std::ptrdiff_t>(offset),
                           in.begin() + static_cast<std::ptrdiff_t>(offset + length));
                offset += length;
                return true;
            }

            static bool readChunk(const std::vector<uint8_t>& in, size_t& offset, std::string& out) {
                std::vector<uint8_t> bytes;
                if (!readChunk(in, offset, bytes))
                    return false;
                out.assign(bytes.begin(), bytes.end());
                return true;
            }

            static constexpr uint32_t kFragmentSlotVersion = 1;

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

                // An ARA plug-in editing a region changes what the track
                // renders, exactly as a user edit in our own UI would, so it
                // must revoke a frozen render the same way. Without this a
                // frozen track keeps playing pre-edit audio with nothing to
                // indicate why.
                controller->setRenderedSignalChangedCallback(
                    [this](const ProjectObjectId& trackId) {
                        // trackIndexForReferenceId scans regular tracks only and
                        // returns -1 both for the master track and for an id it
                        // does not know, so the master case is resolved first
                        // rather than inferred from that -1.
                        auto* master = engine_.timeline().masterTimelineTrack();
                        const auto trackIndex = master && master->referenceId() == trackId
                            ? kMasterTrackIndex
                            : engine_.timeline().trackIndexForReferenceId(trackId);
                        if (trackIndex < 0 && trackIndex != kMasterTrackIndex)
                            return;
                        engine_.frozenTrackManager().projectTrackBecameDirty(trackIndex);
                    });

                if (!controller->resyncFromProjectDocument(
                        engine_.timeline().projectDocumentView(),
                        engine_.timeline().buildMasterTrackSnapshot()))
                    return AraStatus::BackendError;

                if (auto pendingIt = pending_native_archives_.find(pluginInstanceId); pendingIt != pending_native_archives_.end()) {
                    if (!controller->loadArchiveState(pendingIt->second.bytes, pendingIt->second.archive_id))
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
                    std::string archiveId;
                    if (!document.controller->saveArchiveState(archive, archiveId)) {
                        error = std::format("Failed to archive native ARA document for plugin instance {}.", pluginInstanceId);
                        return false;
                    }
                    if (archive.empty())
                        continue;

                    const auto archivePath = std::filesystem::path("native") / (std::to_string(archiveIndex++) + ".bin");
                    if (!context.writeExtensionFile(extensionId(), archivePath, archive, error))
                        return false;
                    // Archive id before the node id, which runs to end of line
                    // so that it may contain spaces.
                    manifest << "native " << archivePath.generic_string()
                             << " " << archiveId << " " << nodeId << "\n";

                }

                return context.writeExtensionFile(
                    extensionId(),
                    "manifest.txt",
                    bytesFromString(manifest.str()),
                    error);
            }

            bool captureClipFragmentState(
                const ProjectObjectId& clipId,
                std::vector<uint8_t>& state,
                std::string& error) override {
                std::vector<uint8_t> entries;
                uint32_t entryCount = 0;

                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    if (!document.controller)
                        continue;
                    const auto nodeId = nodeIdForInstance(pluginInstanceId);
                    if (nodeId.empty())
                        continue;

                    std::string archiveId;
                    std::string audioSourceId;
                    std::string modificationId;
                    std::vector<uint8_t> archive;
                    if (!document.controller->storeArchiveStateForClip(
                            clipId, archiveId, audioSourceId, modificationId, archive)) {
                        error = std::format(
                            "Failed to archive ARA state for clip {} from plugin instance {}. "
                            "Archiving is not permitted while the document is being edited.",
                            clipId, pluginInstanceId);
                        return false;
                    }
                    if (archive.empty())
                        continue;

                    appendChunk(entries, nodeId);
                    appendChunk(entries, archiveId);
                    appendChunk(entries, audioSourceId);
                    appendChunk(entries, modificationId);
                    appendChunk(entries, archive.data(), archive.size());
                    ++entryCount;
                }

                if (entryCount == 0)
                    return true;

                const auto* versionBytes = reinterpret_cast<const uint8_t*>(&kFragmentSlotVersion);
                state.insert(state.end(), versionBytes, versionBytes + sizeof(kFragmentSlotVersion));
                const auto* countBytes = reinterpret_cast<const uint8_t*>(&entryCount);
                state.insert(state.end(), countBytes, countBytes + sizeof(entryCount));
                state.insert(state.end(), entries.begin(), entries.end());
                return true;
            }

            bool restoreClipFragmentState(
                const ProjectObjectId& clipId,
                const std::vector<uint8_t>& state,
                std::string& error) override {
                if (state.empty())
                    return true;

                size_t offset = 0;
                uint32_t version{};
                uint32_t entryCount{};
                if (state.size() < sizeof(version) + sizeof(entryCount)) {
                    error = "Malformed ARA fragment state.";
                    return false;
                }
                std::memcpy(&version, state.data(), sizeof(version));
                offset += sizeof(version);
                std::memcpy(&entryCount, state.data() + offset, sizeof(entryCount));
                offset += sizeof(entryCount);
                if (version != kFragmentSlotVersion) {
                    error = std::format("Unsupported ARA fragment state version {}.", version);
                    return false;
                }

                for (uint32_t i = 0; i < entryCount; ++i) {
                    std::string nodeId;
                    std::string archiveId;
                    std::string audioSourceId;
                    std::string modificationId;
                    std::vector<uint8_t> archive;
                    if (!readChunk(state, offset, nodeId)
                        || !readChunk(state, offset, archiveId)
                        || !readChunk(state, offset, audioSourceId)
                        || !readChunk(state, offset, modificationId)
                        || !readChunk(state, offset, archive)) {
                        error = "Truncated ARA fragment state.";
                        return false;
                    }

                    // The plug-in that produced this entry may not be present
                    // any more -- the fragment could have come from another
                    // project, or its plug-in since removed. Skipping is
                    // correct; there is nothing to restore the state onto.
                    const auto instanceId = instanceIdForNodeId(nodeId);
                    if (instanceId < 0)
                        continue;
                    auto documentIt = native_ara_documents_.find(instanceId);
                    if (documentIt == native_ara_documents_.end() || !documentIt->second.controller)
                        continue;

                    if (!documentIt->second.controller->restoreArchiveStateForClip(
                            clipId, archiveId, audioSourceId, modificationId, archive)) {
                        error = std::format(
                            "Failed to restore ARA state for clip {} onto plugin instance {}.",
                            clipId, instanceId);
                        return false;
                    }
                }
                return true;
            }

            bool captureTrackFragmentState(
                const ProjectObjectId& trackId,
                std::vector<uint8_t>& state,
                std::string& error) override {
                std::vector<uint8_t> entries;
                uint32_t entryCount = 0;

                for (auto& [pluginInstanceId, document] : native_ara_documents_) {
                    if (!document.controller)
                        continue;
                    const auto nodeId = nodeIdForInstance(pluginInstanceId);
                    if (nodeId.empty())
                        continue;

                    std::string archiveId;
                    std::string regionSequenceId;
                    std::vector<uint8_t> archive;
                    if (!document.controller->storeArchiveStateForTrack(
                            trackId, archiveId, regionSequenceId, archive)) {
                        error = std::format(
                            "Failed to archive ARA state for track {} from plugin instance {}. "
                            "Archiving is not permitted while the document is being edited.",
                            trackId, pluginInstanceId);
                        return false;
                    }
                    if (archive.empty())
                        continue;

                    appendChunk(entries, nodeId);
                    appendChunk(entries, archiveId);
                    appendChunk(entries, regionSequenceId);
                    appendChunk(entries, archive.data(), archive.size());
                    ++entryCount;
                }

                if (entryCount == 0)
                    return true;

                const auto* versionBytes = reinterpret_cast<const uint8_t*>(&kFragmentSlotVersion);
                state.insert(state.end(), versionBytes, versionBytes + sizeof(kFragmentSlotVersion));
                const auto* countBytes = reinterpret_cast<const uint8_t*>(&entryCount);
                state.insert(state.end(), countBytes, countBytes + sizeof(entryCount));
                state.insert(state.end(), entries.begin(), entries.end());
                return true;
            }

            bool restoreTrackFragmentState(
                const ProjectObjectId& trackId,
                const std::vector<uint8_t>& state,
                std::string& error) override {
                if (state.empty())
                    return true;

                size_t offset = 0;
                uint32_t version{};
                uint32_t entryCount{};
                if (state.size() < sizeof(version) + sizeof(entryCount)) {
                    error = "Malformed ARA track fragment state.";
                    return false;
                }
                std::memcpy(&version, state.data(), sizeof(version));
                offset += sizeof(version);
                std::memcpy(&entryCount, state.data() + offset, sizeof(entryCount));
                offset += sizeof(entryCount);
                if (version != kFragmentSlotVersion) {
                    error = std::format("Unsupported ARA track fragment state version {}.", version);
                    return false;
                }

                for (uint32_t i = 0; i < entryCount; ++i) {
                    std::string nodeId;
                    std::string archiveId;
                    std::string regionSequenceId;
                    std::vector<uint8_t> archive;
                    if (!readChunk(state, offset, nodeId)
                        || !readChunk(state, offset, archiveId)
                        || !readChunk(state, offset, regionSequenceId)
                        || !readChunk(state, offset, archive)) {
                        error = "Truncated ARA track fragment state.";
                        return false;
                    }

                    const auto instanceId = instanceIdForNodeId(nodeId);
                    if (instanceId < 0)
                        continue;
                    auto documentIt = native_ara_documents_.find(instanceId);
                    if (documentIt == native_ara_documents_.end() || !documentIt->second.controller)
                        continue;

                    if (!documentIt->second.controller->restoreArchiveStateForTrack(
                            trackId, archiveId, regionSequenceId, archive)) {
                        error = std::format(
                            "Failed to restore ARA state for track {} onto plugin instance {}.",
                            trackId, instanceId);
                        return false;
                    }
                }
                return true;
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
                    std::string archiveId;
                    if (keyedByNodeId) {
                        std::string nodeId;
                        if (!(manifest >> archivePath >> archiveId) || !std::getline(manifest, nodeId)) {
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
                        if (!documentIt->second.controller->loadArchiveState(*archiveBytes, archiveId)) {
                            error = std::format("Failed to restore native ARA document for plugin instance {}.", pluginInstanceId);
                            return false;
                        }
                    } else {
                        pending_native_archives_[pluginInstanceId] =
                            PendingArchive{std::move(archiveId), std::move(*archiveBytes)};
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
