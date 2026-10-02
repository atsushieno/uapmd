#pragma once

#include "uapmd-ara/uapmd-ara.hpp"

namespace uapmd::ara {

    // Common document operations for SDK bindings and the AAP-native transport.
    // Native SDK handles are absent for an AAP document.
    class AraDocumentController {
    public:
        virtual ~AraDocumentController() = default;
        virtual bool valid() const = 0;
        virtual ARA::ARADocumentControllerRef documentControllerRef() const = 0;
        virtual const ARA::ARAFactory* factory() const = 0;
        virtual void bindPluginExtension(const ARA::ARAPlugInExtensionInstance* pluginExtension) = 0;
        virtual bool resyncFromProjectDocument(
            ProjectDocumentView& documentView,
            const TimelineFacade::MasterTrackSnapshot& masterTrackSnapshot) = 0;
        virtual bool applyProjectDocumentEvent(
            ProjectDocumentView& documentView,
            const TimelineFacade::MasterTrackSnapshot& masterTrackSnapshot,
            const ProjectDocumentEvent& event) = 0;
        // Holds one ARA edit cycle open across a batch of document events, so
        // that a multi-step edit reaches the plug-in atomically instead of as
        // one cycle per event. Calls nest.
        virtual void beginProjectDocumentTransaction() = 0;
        virtual void endProjectDocumentTransaction() = 0;
        virtual AraRequestId requestAnalysis(AraAnalysisRequest request, AraAnalysisCallback callback) = 0;
        // Reads content for one object, keeping ARA's required call sequence
        // atomic with respect to other calls on this document controller.
        virtual std::optional<AraContentEvents> readContent(
            AraContentScope scope,
            const ProjectObjectId& objectId,
            AraContentKind kind) = 0;
        virtual void cancelAnalysis(AraRequestId requestId) = 0;
        // `archiveId` receives the plug-in factory's document archive
        // identifier. ARA requires it to be stored with the archive and passed
        // back to loadArchiveState, which refuses archives the plug-in does not
        // declare as its own or compatible.
        virtual bool saveArchiveState(std::vector<uint8_t>& archive, std::string& archiveId) = 0;
        // Invoked when a plug-in edit changes what a track renders. An ARA
        // edit is a user edit as far as anything caching rendered audio is
        // concerned, so the host must invalidate those caches just as it does
        // for an edit made in its own UI.
        virtual void setRenderedSignalChangedCallback(
            std::function<void(const ProjectObjectId& trackId)> callback) = 0;


        // Partial archive covering only the ARA objects belonging to one clip,
        // for carrying that state inside a document fragment. The returned
        // persistent IDs identify what the archive was taken from, and must be
        // handed back on restore so the plug-in can be told which current
        // objects they correspond to.
        //
        // Archiving is illegal while the document is being edited, so this must
        // not be called from inside a document transaction.
        virtual bool storeArchiveStateForClip(
            const ProjectObjectId& clipId,
            std::string& archiveId,
            std::string& archivedAudioSourcePersistentId,
            std::string& archivedAudioModificationPersistentId,
            std::vector<uint8_t>& archive) = 0;

        // Track-level counterpart; backends remap the saved modifications
        // onto the pasted track's current objects.
        virtual bool storeArchiveStateForTrack(
            const ProjectObjectId& trackId,
            std::string& archiveId,
            std::string& archivedRegionSequencePersistentId,
            std::vector<uint8_t>& archive) = 0;

        virtual bool restoreArchiveStateForTrack(
            const ProjectObjectId& trackId,
            const std::string& archiveId,
            const std::string& archivedRegionSequencePersistentId,
            const std::vector<uint8_t>& archive) = 0;

        virtual bool restoreArchiveStateForClip(
            const ProjectObjectId& clipId,
            const std::string& archiveId,
            const std::string& archivedAudioSourcePersistentId,
            const std::string& archivedAudioModificationPersistentId,
            const std::vector<uint8_t>& archive) = 0;
        virtual bool loadArchiveState(const std::vector<uint8_t>& archive, const std::string& archiveId) = 0;
        virtual void notifyModelUpdates() = 0;

    };

} // namespace uapmd::ara
