#if __APPLE__

#include "PluginFormatAU.hpp"

remidy::AudioPluginInstanceAU::ParameterSupport::ParameterSupport(remidy::AudioPluginInstanceAU *owner)
        : owner(owner) {
    auto result = AudioUnitGetPropertyInfo(owner->instance, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &au_param_id_list_size, nil);
    if (result != noErr) {
        owner->logger()->logError("%s: AudioPluginInstanceAU failed to retrieve parameter list. Status: %d", owner->name.c_str(), result);
        return;
    }
    au_param_id_list = static_cast<AudioUnitParameterID *>(calloc(au_param_id_list_size, 1));
    result = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, au_param_id_list, &au_param_id_list_size);
    if (result != noErr) {
        owner->logger()->logError("%s: AudioPluginInstanceAU failed to retrieve parameter list. Status: %d", owner->name.c_str(), result);
        return;
    }

    AudioUnitParameterInfo info;
    for (size_t i = 0, n = au_param_id_list_size / sizeof(AudioUnitParameterID); i < n; i++) {
        auto id = au_param_id_list[i];
        UInt32 size = sizeof(info);
        result = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size);
        if (result != noErr) {
            owner->logger()->logError("%s: AudioPluginInstanceAU failed to retrieve parameter %d (at %d). Status: %d", owner->name.c_str(), id, i, result);
            continue;
        }
        std::string idString = std::format("{}", id);
        std::string name{info.name};
        std::string path{};
        auto p = new PluginParameter(id, idString, name, path, info.defaultValue, info.minValue, info.maxValue, true, false);
        parameter_list.emplace_back(p);
    }

    // FIXME: collect parameter_lists_per_note i.e. per-note parameter controllers, *per note* !
}

remidy::AudioPluginInstanceAU::ParameterSupport::~ParameterSupport() {
    if (au_param_id_list)
        free(au_param_id_list);
}

std::vector<remidy::PluginParameter*>& remidy::AudioPluginInstanceAU::ParameterSupport::parameters() {
    return parameter_list;
}

std::vector<remidy::PluginParameter*>& remidy::AudioPluginInstanceAU::ParameterSupport::perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext context) {
    // FIXME: query requested parameter list per group/channel/note
    return parameter_list;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // FIXME: calculate inBufferOffsetInFrames from timestamp.
    auto inBufferOffsetInFrames = 0;
    AudioUnitSetParameter(owner->instance, au_param_id_list[index], kAudioUnitScope_Global, 0, (float) value, inBufferOffsetInFrames);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value) {
        owner->logger()->logError("AudioPluginInstanceAU::ParameterSupport::getParameter(): value is null");
        return StatusCode::INVALID_PARAMETER_OPERATION;
    }
    AudioUnitParameterValue av;
    AudioUnitGetParameter(owner->instance, au_param_id_list[index], kAudioUnitScope_Global, 0, &av);
    *value = av;
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
    // FIXME: calculate inBufferOffsetInFrames from timestamp.
    auto inBufferOffsetInFrames = 0;
    AudioUnitSetParameter(owner->instance, au_param_id_list[index], kAudioUnitScope_Note, context.note, (float) value, inBufferOffsetInFrames);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double* value) {
    owner->logger()->logInfo("remidy::AudioPluginInstanceAU::getPerNoteController not implemented");
    if (!value) {
        owner->logger()->logError("AudioPluginInstanceAU::ParameterSupport::getPerNoteController(): value is null");
        return StatusCode::INVALID_PARAMETER_OPERATION;
    }
    *value = 0;
    return StatusCode::NOT_IMPLEMENTED;
}
#endif
