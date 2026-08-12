#pragma once

#include <string>
#include <vector>

#include <ARA_API/ARAInterface.h>
#include <uapmd-engine/uapmd-engine.hpp>

#include "uapmd-ara/uapmd-ara.hpp"

namespace uapmd::ara {

    class AraHostDocumentController {
    public:
        struct Impl;

        explicit AraHostDocumentController(const ARA::ARAFactory& factory, std::string documentName);
        ~AraHostDocumentController();

        AraHostDocumentController(const AraHostDocumentController&) = delete;
        AraHostDocumentController& operator=(const AraHostDocumentController&) = delete;

        bool valid() const;
        ARA::ARADocumentControllerRef documentControllerRef() const;
        const ARA::ARAFactory* factory() const;
        void bindPluginExtension(const ARA::ARAPlugInExtensionInstance* pluginExtension);
        bool resyncFromProjectDocument(
            ProjectDocumentView& documentView,
            const TimelineFacade::MasterTrackSnapshot& masterTrackSnapshot);
        bool applyProjectDocumentEvent(
            ProjectDocumentView& documentView,
            const TimelineFacade::MasterTrackSnapshot& masterTrackSnapshot,
            const ProjectDocumentEvent& event);
        // Holds one ARA edit cycle open across a batch of document events, so
        // that a multi-step edit reaches the plug-in atomically instead of as
        // one cycle per event. Calls nest.
        void beginProjectDocumentTransaction();
        void endProjectDocumentTransaction();
        AraRequestId requestAnalysis(AraAnalysisRequest request, AraAnalysisCallback callback);
        void cancelAnalysis(AraRequestId requestId);
        bool saveArchiveState(std::vector<uint8_t>& archive);
        bool loadArchiveState(const std::vector<uint8_t>& archive);
        void notifyModelUpdates();

    private:
        Impl* impl_{};
    };

} // namespace uapmd::ara
