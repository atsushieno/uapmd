#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uapmd/uapmd.hpp>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

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
        , public AudioPluginInstanceExtension {
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

    inline AraPluginExtension* getAraPluginExtension(AudioPluginInstanceAPI& instance) {
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
            AudioPluginInstanceAPI& pluginInstance) = 0;
        virtual void detachPlugin(int32_t pluginInstanceId) = 0;

        virtual AraPluginDocument* pluginDocument(int32_t pluginInstanceId) = 0;
        virtual std::vector<int32_t> attachedPluginInstanceIds() const = 0;

        virtual AraStatus resyncFromProjectDocument() = 0;
    };

} // namespace uapmd
