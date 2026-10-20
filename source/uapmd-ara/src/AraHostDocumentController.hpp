#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <string>
#include <vector>

#include <ARA_API/ARAInterface.h>
#include <uapmd-data/uapmd-data.hpp>

#include "AraDocumentController.hpp"

namespace uapmd::ara {

    class AraHostDocumentController final : public AraDocumentController {
    public:
        struct Impl;

        // Companion transports can supply an SDK controller proxy while reusing
        // the same document projection, host services, and archive implementation.
        using CreateController = std::function<const ARA::ARADocumentControllerInstance*(
            const ARA::ARADocumentControllerHostInstance*, const ARA::ARADocumentProperties*)>;
        enum class ArchiveScope { Document, Clip, Track };
        using ArchiveTarget = std::pair<ProjectObjectId, ARA::ARAAudioModificationRef>;
        using ArchiveImporter = std::function<std::optional<bool>(ArchiveScope,
            const std::string&, const std::vector<uint8_t>&, const std::vector<ArchiveTarget>&)>;

        explicit AraHostDocumentController(const ARA::ARAFactory& factory, std::string documentName);
        AraHostDocumentController(CreateController createController, std::string documentName);
        void setArchiveImporter(ArchiveImporter importer);
        ~AraHostDocumentController() override;

        AraHostDocumentController(const AraHostDocumentController&) = delete;
        AraHostDocumentController& operator=(const AraHostDocumentController&) = delete;

        bool valid() const override;
        bool notifySelection(const ProjectDocumentView& view, const ProjectSelection& selection) override;
        void setEditorViewVisible(bool visible) override;
        ARA::ARADocumentControllerRef documentControllerRef() const override;
        const ARA::ARAFactory* factory() const override;
        void bindPluginExtension(const ARA::ARAPlugInExtensionInstance* pluginExtension) override;
        bool resyncFromProjectDocument(
            const ProjectDocumentView& documentView,
            const ProjectMasterTrackSnapshot& masterTrackSnapshot) override;
        bool applyProjectDocumentEvent(
            const ProjectDocumentView& documentView,
            const ProjectMasterTrackSnapshot& masterTrackSnapshot,
            const ProjectDocumentEvent& event) override;
        // Holds one ARA edit cycle open across a batch of document events, so
        // that a multi-step edit reaches the plug-in atomically instead of as
        // one cycle per event. Calls nest.
        void beginProjectDocumentTransaction() override;
        void endProjectDocumentTransaction() override;
        AraRequestId requestAnalysis(AraAnalysisRequest request, AraAnalysisCallback callback) override;
        // Reads content for one object, keeping ARA's required call sequence
        // atomic with respect to other calls on this document controller.
        std::optional<AraContentEvents> readContent(
            AraContentScope scope,
            const ProjectObjectId& objectId,
            AraContentKind kind) override;
        void cancelAnalysis(AraRequestId requestId) override;
        // `archiveId` receives the plug-in factory's document archive
        // identifier. ARA requires it to be stored with the archive and passed
        // back to loadArchiveState, which refuses archives the plug-in does not
        // declare as its own or compatible.
        bool saveArchiveState(std::vector<uint8_t>& archive, std::string& archiveId) override;
        // True when the plug-in has reported private state changes since the
        // last archive was taken. The host cannot inspect that state, so this
        // is the only signal that an archive of it has gone stale.
        // Invoked when a plug-in edit changes what a track renders. An ARA
        // edit is a user edit as far as anything caching rendered audio is
        // concerned, so the host must invalidate those caches just as it does
        // for an edit made in its own UI.
        void setRenderedSignalChangedCallback(
            std::function<void(const ProjectObjectId& trackId)> callback) override;


        // Partial archive covering only the ARA objects belonging to one clip,
        // for carrying that state inside a document fragment. The returned
        // persistent IDs identify what the archive was taken from, and must be
        // handed back on restore so the plug-in can be told which current
        // objects they correspond to.
        //
        // Archiving is illegal while the document is being edited, so this must
        // not be called from inside a document transaction.
        bool storeArchiveStateForClip(
            const ProjectObjectId& clipId,
            std::string& archiveId,
            std::string& archivedAudioSourcePersistentId,
            std::string& archivedAudioModificationPersistentId,
            std::vector<uint8_t>& archive) override;

        // Track-level counterpart, using the ARA 3.0 draft region sequence
        // entries in the store and restore filters.
        bool storeArchiveStateForTrack(
            const ProjectObjectId& trackId,
            std::string& archiveId,
            std::string& archivedRegionSequencePersistentId,
            std::vector<uint8_t>& archive) override;

        bool restoreArchiveStateForTrack(
            const ProjectObjectId& trackId,
            const std::string& archiveId,
            const std::string& archivedRegionSequencePersistentId,
            const std::vector<uint8_t>& archive) override;

        bool restoreArchiveStateForClip(
            const ProjectObjectId& clipId,
            const std::string& archiveId,
            const std::string& archivedAudioSourcePersistentId,
            const std::string& archivedAudioModificationPersistentId,
            const std::vector<uint8_t>& archive) override;
        bool loadArchiveState(const std::vector<uint8_t>& archive, const std::string& archiveId) override;
        void notifyModelUpdates() override;

    private:
        Impl* impl_{};
    };

} // namespace uapmd::ara
