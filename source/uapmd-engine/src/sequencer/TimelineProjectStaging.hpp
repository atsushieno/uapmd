#pragma once

// Staging structures for an in-flight project save. Plug-in state and graph
// serialization are asynchronous, so a save keeps its partial results here
// until every plug-in has reported.
//
// Private to the timeline facade implementation.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-engine/uapmd-engine.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd::timeline_detail {

    using uapmd_plugin_hosting::AudioPluginInstanceAPI;

    struct PendingProjectPluginState {
        int32_t instance_id{-1};
        size_t plugin_order{0};
        AudioPluginInstanceAPI* instance{};
        std::function<void(const std::string& relativePath)> set_state_file{};
        std::string scope_label;
    };

    struct PendingProjectGraphSave {
        UapmdProjectTrackData* track{};
        SequencerTrack* sequencer_track{};
        std::string scope_label;
    };

    struct PendingProjectSaveContext {
        std::filesystem::path project_file;
        std::filesystem::path project_dir;
        std::filesystem::path plugin_state_dir;
        std::filesystem::path graph_dir;
        std::unique_ptr<UapmdProjectData> project;
        std::vector<PendingProjectPluginState> pending_states;
        std::vector<PendingProjectGraphSave> pending_graphs;
        size_t next_pending_state{0};
        uint64_t history_state_id{0};
        bool emit_document_event{true};
        bool mark_history_saved{true};
        TimelineFacade::ProjectSaveCallback callback;
    };

} // namespace uapmd::timeline_detail
