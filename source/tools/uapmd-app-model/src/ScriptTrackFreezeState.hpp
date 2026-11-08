#pragma once

#include <stdexcept>
#include <string_view>
#include <choc/containers/choc_Value.h>
#include "ScriptDocumentIds.hpp"

// Track freeze state for the JS and MCP APIs, read on the main thread.
namespace uapmd_app::script_track_freeze_state {

    inline int32_t resolve(std::string_view trackId) {
        const auto trackIndex = script_ids::trackIndex(trackId);
        if (!trackIndex)
            throw std::invalid_argument("Unknown trackId: " + std::string(trackId));
        if (*trackIndex < 0)
            throw std::invalid_argument("trackId must not be the master track");
        return *trackIndex;
    }

    inline choc::value::Value get(int32_t trackIndex) {
        auto* engine = AppModel::instance().sequencer().engine();
        if (!engine || trackIndex < 0 ||
            static_cast<size_t>(trackIndex) >= engine->tracks().size())
            throw std::invalid_argument("trackId is invalid");

        auto& manager = engine->frozenTrackManager();
        auto result = choc::value::createObject("TrackFreezeState");
        result.setMember("trackId", script_ids::trackId(trackIndex));
        result.setMember(
            "policy",
            manager.freezePolicyForTrack(trackIndex) ==
                    uapmd::FrozenTrackManager::FreezePolicy::On
                ? "on"
                : "off");
        switch (manager.runtimeStateForTrack(trackIndex)) {
            case uapmd::FrozenTrackManager::RuntimeState::Live:
                result.setMember("runtimeState", "live");
                break;
            case uapmd::FrozenTrackManager::RuntimeState::Rendering:
                result.setMember("runtimeState", "rendering");
                break;
            case uapmd::FrozenTrackManager::RuntimeState::Frozen:
                result.setMember("runtimeState", "frozen");
                break;
            case uapmd::FrozenTrackManager::RuntimeState::Error:
                result.setMember("runtimeState", "error");
                break;
        }
        result.setMember("error", manager.errorMessageForTrack(trackIndex));
        const auto progress = manager.renderProgressForTrack(trackIndex);
        result.setMember("renderProgress", progress ? progress->progress : 0.0);
        result.setMember("renderedSeconds", progress ? progress->renderedSeconds : 0.0);
        result.setMember("totalSeconds", progress ? progress->totalSeconds : 0.0);
        result.setMember(
            "renderedFrames",
            choc::value::createInt64(progress ? progress->renderedFrames : 0));
        result.setMember(
            "totalFrames",
            choc::value::createInt64(progress ? progress->totalFrames : 0));
        if (const auto audio = manager.frozenAudioSummaryForTrack(trackIndex)) {
            auto frozenAudio = choc::value::createObject("FrozenAudio");
            frozenAudio.setMember("startSample", choc::value::createInt64(audio->startSample));
            frozenAudio.setMember("frames", choc::value::createInt64(static_cast<int64_t>(audio->frames)));
            frozenAudio.setMember("channels", static_cast<int32_t>(audio->channels));
            frozenAudio.setMember("peak", static_cast<double>(audio->peak));
            frozenAudio.setMember("rms", audio->rms);
            result.setMember("frozenAudio", frozenAudio);
        } else
            result.setMember("frozenAudio", choc::value::Value{});
        return result;
    }

    inline choc::value::Value get(std::string_view trackId) {
        return get(resolve(trackId));
    }

    // Rendering starts asynchronously; poll get() for its progress and result.
    inline choc::value::Value set(int32_t trackIndex, std::string_view policy) {
        if (policy != "on" && policy != "off")
            throw std::invalid_argument("policy must be off or on");
        auto* engine = AppModel::instance().sequencer().engine();
        if (!engine || trackIndex < 0 ||
            static_cast<size_t>(trackIndex) >= engine->tracks().size())
            throw std::invalid_argument("trackId is invalid");
        engine->commands().setTrackFreezePolicyEnabled(trackIndex, policy == "on");
        return get(trackIndex);
    }

    inline choc::value::Value set(std::string_view trackId, std::string_view policy) {
        return set(resolve(trackId), policy);
    }

}
