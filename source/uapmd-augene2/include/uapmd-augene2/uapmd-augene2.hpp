#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace uapmd { class TimelineFacade; }
namespace uapmd_addin { class PanelRegistry; }

namespace uapmd_augene2 {

// An MML resource of the project, as the integration panel lists it.
struct IntegrationSource {
    std::string path;          // Project-relative resource path.
    std::string external_path; // Linked file; empty for a bundled copy.
    bool compile{false};       // Compiled on its own, rather than only #included.
};

// Where the clips generated from one MML track currently live.
struct IntegrationTrackMapping {
    std::string key;
    // A timeline track index, uapmd::kMasterTrackIndex, or -1 when the track
    // is no longer available.
    int32_t track_index{-1};
};

// The Augene2 project service and everything its panel presents, so that a
// host can present it with any toolkit. Every member runs on the model thread.
// Requests complete asynchronously: picking, compilation, and applying the
// result to the timeline advance in PanelRegistry::update().
class Integration {
public:
    virtual ~Integration() = default;

    // Panel visibility. The addin command opens it; disabling the addin closes it.
    virtual bool isOpen() const = 0;
    virtual void setOpen(bool open) = 0;

    // True while a document pick, an apply, or another history operation is in
    // progress. Editing requests are ignored meanwhile.
    virtual bool busy() const = 0;
    // True while sources are being checked or compiled.
    virtual bool compiling() const = 0;

    virtual std::vector<IntegrationSource> sources() const = 0;
    virtual std::vector<IntegrationTrackMapping> trackMappings() const = 0;
    virtual std::string status() const = 0;
    virtual std::vector<std::string> diagnostics() const = 0;

    // Optional project-relative folder that imported resources are placed under.
    virtual std::string resourceFolder() const = 0;
    virtual void setResourceFolder(const std::string& folder) = 0;

    // Picks MML files through the application's document provider. Inputs
    // (compile = true) are compiled; the others are only available to #include.
    virtual void importSources(bool compile) = 0;
    // Picks a replacement file for the resource at `path`.
    virtual void relinkSource(const std::string& path) = 0;
    virtual void removeSource(const std::string& path) = 0;
    // Refreshes linked files, compiles, and applies the result as one undo step.
    virtual void compile() = 0;
};

// Project persistence is available even when the addin's UI/watch service is
// disabled. The application releases it before destroying the timeline.
void registerProjectService(uapmd::TimelineFacade& timeline, uapmd_addin::PanelRegistry& panels);

// The project service registered by registerProjectService() or by the addin,
// or null when there is none or it has been released.
std::shared_ptr<Integration> integration();

}
