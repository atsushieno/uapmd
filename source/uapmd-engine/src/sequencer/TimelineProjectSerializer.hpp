#pragma once

// Reading and writing .uapmd projects.
//
// A save has to wait for every plug-in to hand back its opaque state and for
// every graph to be written; a load has to recreate tracks, clips and plug-ins
// under the identities they were saved with. Neither is a timeline concern,
// so this owns that work and reaches back into the timeline only through the
// public TimelineFacade surface plus the small Host interface below.
//
// Private to the timeline facade implementation.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-engine/uapmd-engine.hpp"
#include "TimelineProjectStaging.hpp"

namespace uapmd::timeline_detail {

    // State shared by the phases of one project load, defined in the
    // implementation file because nothing outside it needs the details.
    struct ProjectLoadRun;
    struct ProjectSaveBuild;

    // Reports one save's outcome exactly once, from whichever asynchronous
    // branch finishes first.
    using ProjectSaveCompletion = std::function<void(TimelineFacade::ProjectResult)>;

    // What a serializer needs from the timeline that TimelineFacade does not
    // already expose. Everything here is either timeline-private state or a
    // mode that only makes sense while a project is being read or written.
    class TimelineProjectSerializerHost {
    public:
        virtual ~TimelineProjectSerializerHost() = default;

        virtual double sampleRate() const = 0;
        virtual uint32_t bufferSizeInFrames() const = 0;

        // Extensions are registered on the timeline rather than here because
        // clip and track fragments contribute through them too.
        virtual std::vector<ProjectSerializationExtension*> serializationExtensions() const = 0;

        virtual void emitProjectDocumentEvent(ProjectDocumentEvent event) = 0;
        virtual void emitMasterTrackChanged(std::string type) = 0;
        virtual void notifyTimelineChanged() = 0;

        // A load recreates objects under the identities they were saved with,
        // by staging the next identity before asking the timeline to create
        // the object. Staging an empty string means "allocate as usual".
        virtual void stageTrackReferenceId(std::string referenceId) = 0;
        virtual void stageClipReferenceId(std::string referenceId) = 0;
        virtual void resetTrackReferenceAllocator() = 0;

        // While a load runs, document events and timeline notifications are
        // suppressed: the project is being established, not edited. This is a
        // flag rather than a scope because the load has several asynchronous
        // exits; giving it a single exit would let it become a scoped guard.
        virtual void setLoadInProgress(bool value) = 0;

        // Bracket plug-in state restoration, so that state notifications
        // coming back from a plug-in are not mistaken for external edits.
        virtual void beginPluginStateRestore() = 0;
        virtual void endPluginStateRestore() = 0;

        // A load starts from a fresh master track rather than clearing the
        // existing one in place.
        virtual void replaceMasterTimelineTrack(std::shared_ptr<TimelineTrack> track) = 0;

        // The clip-creation path that carries markers and warp points. It is
        // not on the public facade because only a load and the fragment code
        // need to supply them at creation time.
        virtual TimelineFacade::ClipAddResult addAudioClipToTimelineTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps) = 0;
    };

    class TimelineProjectSerializer {
        SequencerEngine& engine_;
        TimelineFacade& facade_;
        TimelineProjectSerializerHost& host_;

    public:
        TimelineProjectSerializer(
            SequencerEngine& engine,
            TimelineFacade& facade,
            TimelineProjectSerializerHost& host)
            : engine_(engine)
            , facade_(facade)
            , host_(host) {
        }

        TimelineProjectSerializer(const TimelineProjectSerializer&) = delete;
        TimelineProjectSerializer& operator=(const TimelineProjectSerializer&) = delete;

        void saveProject(
            const std::filesystem::path& file,
            TimelineFacade::ProjectSaveOptions options,
            TimelineFacade::ProjectSaveCallback callback);
        void loadProject(
            const std::filesystem::path& projectFile,
            TimelineFacade::ProjectLoadCallback callback);

        bool materializeProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            size_t eventBufferSizeInBytes);
        bool saveProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            const std::filesystem::path& projectDir,
            const std::filesystem::path& graphDir,
            const std::string& scopeLabel,
            std::string& error);

        bool saveProjectDataExtensions(UapmdProjectData& project, std::string& error);
        bool loadProjectDataExtensions(UapmdProjectData& project, std::string& error);
        bool saveProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error);
        bool loadProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error);

    private:
        // One project save, in order. The document is built synchronously;
        // everything after that waits on plug-ins reporting their state.
        bool beginProjectSave(PendingProjectSaveContext& operation, std::string& error);
        bool buildProjectDocument(
            PendingProjectSaveContext& operation,
            const TimelineFacade::ProjectSaveOptions& options,
            std::string& error);
        bool serializeTracks(
            PendingProjectSaveContext& operation,
            const TimelineFacade::ProjectSaveOptions& options,
            ProjectSaveBuild& build,
            std::string& error);
        bool serializeMasterTrack(
            PendingProjectSaveContext& operation,
            ProjectSaveBuild& build,
            std::string& error);
        void applySerializedClipAnchors(ProjectSaveBuild& build);
        void runPendingPluginStateCaptures(
            std::shared_ptr<PendingProjectSaveContext> operation,
            ProjectSaveCompletion complete);
        void writeSerializedProject(
            std::shared_ptr<PendingProjectSaveContext> operation,
            const ProjectSaveCompletion& complete);
        void finishSuccessfulSave(
            PendingProjectSaveContext& operation,
            const ProjectSaveCompletion& complete);

        // One project load, in order. Each phase reads and extends the run
        // state; a phase that finds run.error already set does nothing, which
        // is how a failure part-way through stops the rest without unwinding.
        bool beginProjectLoad(ProjectLoadRun& run);
        void resetDocumentForLoad(ProjectLoadRun& run);
        void queuePluginLoadsForTrack(
            ProjectLoadRun& run,
            UapmdProjectTrackData* projectTrack,
            int32_t trackIndex);
        void restoreLoadedPluginState(
            int32_t instanceId,
            const std::string& instantiationError,
            const std::filesystem::path& stateFile,
            int32_t groupIndex,
            const std::string& pluginLabel,
            const std::string& pluginId,
            const std::string& format);
        void restoreProjectTracks(ProjectLoadRun& run);
        bool restoreTrackClip(
            ProjectLoadRun& run,
            UapmdProjectClipData& clip,
            int32_t trackIndex);
        void restoreMasterTrackClips(ProjectLoadRun& run);
        void applyLoadedClipAnchors(ProjectLoadRun& run);
        void installLoadCompletion(ProjectLoadRun& run);
        void finalizeLoadedProject(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            const std::shared_ptr<UapmdProjectData>& project,
            std::vector<ClipMarker> masterTrackMarkers,
            const TimelineFacade::ProjectLoadCallback& callback);
        void runQueuedPluginLoads(ProjectLoadRun& run);

        void queueProjectGraphSerialization(
            PendingProjectSaveContext& operation,
            SequencerTrack* sequencerTrack,
            UapmdProjectTrackData& projectTrack,
            const std::string& scopeLabel);
    };

} // namespace uapmd::timeline_detail
