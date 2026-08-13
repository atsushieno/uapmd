#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ARA_API/ARAInterface.h>
#include <uapmd-engine/uapmd-engine.hpp>
#include "ara-plugin-instance-handles.hpp"

namespace uapmd::ara {

    inline constexpr std::string_view kAraPluginExtensionId =
        "dev.atsushieno.uapmd.ara.plugin.v1";

    using AraRequestId = uint64_t;

    enum class AraStatus {
        Ok,
        UnsupportedPlugin,
        InvalidDocument,
        InvalidObject,
        BackendError,
        Cancelled
    };

    enum class AraContentKind {
        Unknown,
        AudioSourceSamples,
        AudioSourcePeaks,
        TempoMap,
        TimeSignatures,
        ClipMarkers,
        AudioWarps,
        Notes,
        Chords,
        Keys,
        Lyrics
    };

    struct AraAnalysisRequest {
        ProjectObjectId objectId;
        std::vector<AraContentKind> contentKinds;
    };

    struct AraAnalysisResult {
        AraRequestId requestId{0};
        ProjectObjectId objectId;
        std::vector<AraContentKind> completedKinds;
        AraStatus status{AraStatus::Ok};
        std::string error;
    };

    using AraAnalysisCallback = std::function<void(const AraAnalysisResult& result)>;

    // Which object's content to read. The level is not interchangeable: since
    // ARA 2, a playback region's content cannot be derived from its audio
    // modification plus transformation flags, because region transitions adjust
    // notes at region borders. Read at region level for what a region plays,
    // and at source or modification level for content in its original,
    // untransformed state -- which is what detection wants.
    enum class AraContentScope {
        // Addressed by audio source id.
        AudioSource,
        // Addressed by audio source id; reads the modification layered on it.
        AudioModification,
        // Addressed by clip id.
        PlaybackRegion
    };

    // How far the plug-in has got with this content. Initial means it has not
    // analysed yet and is guessing.
    enum class AraContentGrade {
        Initial,
        Detected,
        Adjusted,
        Approved
    };

    struct AraContentNote {
        float frequency{0.0f};
        int32_t pitchNumber{0};
        float volume{0.0f};
        double startPosition{0.0};
        double attackDuration{0.0};
        // To the release point, as with a MIDI note off.
        double noteDuration{0.0};
        // To the end of the release phase, so beyond noteDuration.
        double signalDuration{0.0};
    };

    struct AraContentTempoEntry {
        double timePosition{0.0};
        double quarterPosition{0.0};
    };

    struct AraContentBarSignature {
        int32_t numerator{4};
        int32_t denominator{4};
        double position{0.0};
    };

    // Content copied out of a plug-in's reader. The vectors are owned here:
    // reader data is only valid until the reader is destroyed, so it is copied
    // rather than referenced.
    struct AraContentEvents {
        AraContentKind kind{AraContentKind::Unknown};
        AraContentGrade grade{AraContentGrade::Initial};
        std::vector<AraContentNote> notes;
        std::vector<AraContentTempoEntry> tempoEntries;
        std::vector<AraContentBarSignature> barSignatures;

        bool empty() const {
            return notes.empty() && tempoEntries.empty() && barSignatures.empty();
        }
    };

    class AraPluginDocument {
    public:
        virtual ~AraPluginDocument() = default;

        virtual AraStatus bindProjectDocument(
            ProjectDocumentView& documentView,
            ProjectDocumentEventSource& eventSource) = 0;
        virtual void unbindProjectDocument() = 0;

        virtual AraStatus resyncFromProjectDocument() = 0;

        virtual std::vector<uint8_t> saveAraState() = 0;
        virtual AraStatus loadAraState(const std::vector<uint8_t>& state) = 0;

        virtual AraRequestId requestAnalysis(
            AraAnalysisRequest request,
            AraAnalysisCallback callback) = 0;
        virtual void cancelAnalysis(AraRequestId requestId) = 0;
    };

    class AraPluginExtension
        : public remidy::PluginExtensibility<remidy::PluginInstance>
        , public uapmd_plugin_hosting::AudioPluginInstanceExtension {
    protected:
        explicit AraPluginExtension(remidy::PluginInstance& owner)
            : remidy::PluginExtensibility<remidy::PluginInstance>(owner) {
        }

    public:
        std::string_view extensionId() const override {
            return kAraPluginExtensionId;
        }

        virtual bool supportsAraDocument() const = 0;
        virtual std::unique_ptr<AraPluginDocument> createAraPluginDocument() = 0;
    };

    inline AraPluginExtension* getAraPluginExtension(uapmd_plugin_hosting::AudioPluginInstanceAPI& instance) {
        auto* extension = instance.extension(kAraPluginExtensionId);
        return dynamic_cast<AraPluginExtension*>(extension);
    }

    class AraSession : public ProjectDocumentEventListener {
    public:
        virtual ~AraSession() = default;

        virtual AraStatus bindProjectDocument(
            ProjectDocumentView& documentView,
            ProjectDocumentEventSource& eventSource) = 0;
        virtual void unbindProjectDocument() = 0;

        virtual AraStatus attachPlugin(
            int32_t pluginInstanceId,
            uapmd_plugin_hosting::AudioPluginInstanceAPI& pluginInstance) = 0;
        virtual void detachPlugin(int32_t pluginInstanceId) = 0;

        virtual AraPluginDocument* pluginDocument(int32_t pluginInstanceId) = 0;
        virtual std::vector<int32_t> attachedPluginInstanceIds() const = 0;

        virtual AraStatus resyncFromProjectDocument() = 0;

        static std::unique_ptr<AraSession> create();
    };

    class AraSupport {
    public:
        virtual ~AraSupport() = default;

        virtual AraSession& session() = 0;
        virtual const AraSession& session() const = 0;

        virtual AraStatus attachPlugin(
            int32_t pluginInstanceId,
            uapmd_plugin_hosting::AudioPluginInstanceAPI& pluginInstance) = 0;
        virtual void detachPlugin(int32_t pluginInstanceId) = 0;

        virtual bool hasNativeAraBinding(int32_t pluginInstanceId) const = 0;
        virtual const ARA::ARAFactory* nativeAraFactory(int32_t pluginInstanceId) const = 0;
        virtual const ARA::ARAPlugInExtensionInstance* bindNativeAraPlugin(
            int32_t pluginInstanceId,
            ARA::ARADocumentControllerRef documentControllerRef,
            ARA::ARAPlugInInstanceRoleFlags knownRoles,
            ARA::ARAPlugInInstanceRoleFlags assignedRoles) = 0;
        virtual AraRequestId requestAnalysis(
            int32_t pluginInstanceId,
            AraAnalysisRequest request,
            AraAnalysisCallback callback) = 0;

        // Reads content a plug-in exposes for one object.
        //
        // Pull, not push: the app learns that something changed from the
        // document and analysis notifications, then reads only when it wants
        // the content. Returns nothing when the plug-in does not offer this
        // content for this object, which a completed analysis does not
        // guarantee -- a plug-in may reject or fail a request, so availability
        // is checked at read time rather than inferred from completion.
        virtual std::optional<AraContentEvents> readContent(
            int32_t pluginInstanceId,
            AraContentScope scope,
            const ProjectObjectId& objectId,
            AraContentKind kind) = 0;
        virtual void cancelAnalysis(int32_t pluginInstanceId, AraRequestId requestId) = 0;
    };

    std::unique_ptr<AraSupport> createAraSupport(SequencerEngine& engine);

} // namespace uapmd::ara
