#pragma once

#include <future>
#include <string>

#include <uapmd-app-model/uapmd-app-model.hpp>

// For the script and MCP entry points, which have to return a result. This is not public API:
// AppModel's own project operations take a callback.
namespace uapmd_app {

    AppModel::ProjectResult waitForProjectResult(std::future<AppModel::ProjectResult>& future);

    // Loads the document behind a token from IDocumentProvider::persistHandle, as the GUI's Load
    // Project does. Call it on the main thread, which is where scripts and MCP tools run.
    AppModel::ProjectResult loadProjectFromHandleTokenAndWait(const std::string& token);

}
