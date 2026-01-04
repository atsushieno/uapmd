#if __APPLE__

#include <format>
#include <algorithm>
#include <unordered_map>
#import <AudioToolbox/AUAudioUnit.h>
#include "PluginFormatAU.hpp"

namespace {

static std::string keyPathToGroupPath(NSString* keyPath) {
    if (!keyPath)
        return {};

    std::string path = cfStringToString((__bridge CFStringRef) keyPath);
    auto pos = path.rfind('.');
    if (pos != std::string::npos)
        path = path.substr(0, pos);
    std::replace(path.begin(), path.end(), '.', '/');
    return path;
}

}

remidy::PluginInstanceAUv2::ParameterSupport::ParameterSupport(remidy::PluginInstanceAUv2 *owner)
        : owner(owner) {
    auto impl = [&] {
    auto result = AudioUnitGetPropertyInfo(owner->instance, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &au_param_id_list_size, nil);
    if (result != noErr) {
        owner->logger()->logError("%s: PluginInstanceAU failed to retrieve parameter list. Status: %d", owner->name.c_str(), result);
        return;
    }
    au_param_id_list = static_cast<AudioUnitParameterID *>(calloc(au_param_id_list_size, 1));
    result = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, au_param_id_list, &au_param_id_list_size);
    if (result != noErr) {
        owner->logger()->logError("%s: PluginInstanceAU failed to retrieve parameter list. Status: %d", owner->name.c_str(), result);
        return;
    }

    AudioUnitParameterInfo info;
    AUParameterTree* parameterTree = nullptr;
    std::unordered_map<UInt32, std::string> clumpNames;
    auto getClumpPath = [&](UInt32 clumpId) -> std::string {
        if (clumpId == 0)
            return {};
        auto found = clumpNames.find(clumpId);
        if (found != clumpNames.end())
            return found->second;

        AudioUnitParameterNameInfo clumpNameInfo{};
        clumpNameInfo.inID = clumpId;
        clumpNameInfo.inDesiredLength = kAudioUnitParameterName_Full;
        UInt32 clumpSize = sizeof(clumpNameInfo);
        std::string name{};
        if (AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterClumpName,
                                 kAudioUnitScope_Global, 0, &clumpNameInfo, &clumpSize) == noErr &&
            clumpNameInfo.outName) {
            name = cfStringToString(clumpNameInfo.outName);
            CFRelease(clumpNameInfo.outName);
        }
        if (name.empty())
            name = std::to_string(clumpId);
        clumpNames.emplace(clumpId, name);
        return name;
    };

    for (size_t i = 0, n = au_param_id_list_size / sizeof(AudioUnitParameterID); i < n; i++) {
        auto id = au_param_id_list[i];
        UInt32 size = sizeof(info);
        result = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size);
        if (result != noErr) {
            owner->logger()->logError("%s: PluginInstanceAU failed to retrieve parameter %d (at %d). Status: %d", owner->name.c_str(), id, i, result);
            continue;
        }
        std::string idString = std::format("{}", id);
        char nameBuffer[512];
        CFStringGetCString(info.cfNameString, nameBuffer, sizeof(nameBuffer), kCFStringEncodingUTF8);
        std::string pName{nameBuffer};
        std::string path{};
        if (parameterTree) {
            AUParameter* param = [parameterTree parameterWithID:id scope:kAudioUnitScope_Global element:0];
            if (param)
                path = keyPathToGroupPath(param.keyPath);
        }
        if (path.empty() && (info.flags & kAudioUnitParameterFlag_HasClump))
            path = getClumpPath(info.clumpID);

        // Retrieve enumeration strings before creating parameter
        CFArrayRef enumArray{nullptr};
        std::vector<ParameterEnumeration> enums;
        UInt32 enumSize = sizeof(enumArray);
        result = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &enumArray, &enumSize);
        if (result == noErr && enumArray) {
            auto count = CFArrayGetCount(enumArray);
            enums.reserve(static_cast<size_t>(count));
            for (CFIndex e = 0; e < count; e++) {
                auto str = static_cast<CFStringRef>(CFArrayGetValueAtIndex(enumArray, e));
                auto label = cfStringToString(str);
                if (label.empty())
                    label = std::format("#{}", e);
                enums.emplace_back(label, e);
            }
            CFRelease(enumArray);
        }

        auto p = new PluginParameter(i, idString, pName, path, info.defaultValue, info.minValue, info.maxValue,
                                     (info.flags & kAudioUnitParameterFlag_NonRealTime) == 0,
                                     info.flags & kAudioUnitParameterFlag_IsReadable,
                                     false, // I don't see `hidden` flag for AU parameters
                                     info.unit == kAudioUnitParameterUnit_Indexed,
                                     enums
                                     );
        parameter_list.emplace_back(p);
        parameter_id_to_index[id] = static_cast<uint32_t>(i);
    }

    if (parameterTree)
        [parameterTree release];

    // FIXME: collect parameter_lists_per_note i.e. per-note parameter controllers, *per note* !

    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::Parameters)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();

    installParameterListener();
}

remidy::PluginInstanceAUv2::ParameterSupport::~ParameterSupport() {
    uninstallParameterListener();
    if (au_param_id_list)
        free(au_param_id_list);
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceAUv2::ParameterSupport::parameters() {
    return parameter_list;
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceAUv2::ParameterSupport::perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext context) {
    auto scopeInfo = scopeFromContext(types, context);
    if (!scopeInfo.has_value())
        return parameter_list;

    const auto scope = scopeInfo->first;
    const auto element = scopeInfo->second;
    scoped_parameter_list.clear();

    auto populate = [&] {
        scoped_parameter_list = buildScopedParameterList(scope, element);
    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::Parameters)
        EventLoop::runTaskOnMainThread(populate);
    else
        populate();

    return scoped_parameter_list;
}

std::vector<remidy::PluginParameter*> remidy::PluginInstanceAUv2::ParameterSupport::buildScopedParameterList(AudioUnitScope scope, UInt32 element) {
    std::vector<PluginParameter*> scoped{};

    UInt32 listSize = 0;
    auto status = AudioUnitGetPropertyInfo(owner->instance, kAudioUnitProperty_ParameterList, scope, element, &listSize, nil);
    if (status != noErr) {
        if (owner->logger())
            owner->logger()->logError("%s: failed to query scoped parameter info (scope=%u, element=%u). Status: %d",
                owner->name.c_str(), scope, element, status);
        return scoped;
    }
    if (listSize == 0)
        return scoped;

    const auto parameterCount = listSize / sizeof(AudioUnitParameterID);
    if (parameterCount == 0)
        return scoped;

    std::vector<AudioUnitParameterID> scopedIds(parameterCount);
    status = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterList, scope, element, scopedIds.data(), &listSize);
    if (status != noErr) {
        if (owner->logger())
            owner->logger()->logError("%s: failed to retrieve scoped parameter list (scope=%u, element=%u). Status: %d",
                owner->name.c_str(), scope, element, status);
        return {};
    }

    scoped.reserve(parameterCount);
    for (auto id : scopedIds) {
        if (auto index = indexForParameterId(id); index.has_value())
            scoped.emplace_back(parameter_list[index.value()]);
    }
    return scoped;
}

std::optional<std::pair<AudioUnitScope, UInt32>> remidy::PluginInstanceAUv2::ParameterSupport::scopeFromContext(PerNoteControllerContextTypes types, PerNoteControllerContext context) const {
    if (types & PER_NOTE_CONTROLLER_PER_NOTE)
        return std::make_pair(kAudioUnitScope_Note, context.note);
    if (types & PER_NOTE_CONTROLLER_PER_CHANNEL)
        return std::make_pair(kAudioUnitScope_Part, context.channel);
    if (types & PER_NOTE_CONTROLLER_PER_GROUP)
        return std::make_pair(kAudioUnitScope_Group, context.group);
    return std::nullopt;
}

remidy::StatusCode remidy::PluginInstanceAUv2::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // FIXME: calculate inBufferOffsetInFrames from timestamp.
    auto inBufferOffsetInFrames = 0;
    AudioUnitSetParameter(owner->instance, au_param_id_list[index], kAudioUnitScope_Global, 0, (float) value, inBufferOffsetInFrames);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv2::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value) {
        owner->logger()->logError("PluginInstanceAUv2::ParameterSupport::getParameter(): value is null");
        return StatusCode::INVALID_PARAMETER_OPERATION;
    }
    AudioUnitParameterValue av;
    AudioUnitGetParameter(owner->instance, au_param_id_list[index], kAudioUnitScope_Global, 0, &av);
    *value = av;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv2::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
    // FIXME: calculate inBufferOffsetInFrames from timestamp.
    auto inBufferOffsetInFrames = 0;
    AudioUnitSetParameter(owner->instance, au_param_id_list[index], kAudioUnitScope_Note, context.note, (float) value, inBufferOffsetInFrames);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv2::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double* value) {
    owner->logger()->logInfo("remidy::PluginInstanceAUv2::getPerNoteController not implemented");
    if (!value) {
        owner->logger()->logError("PluginInstanceAUv2::ParameterSupport::getPerNoteController(): value is null");
        return StatusCode::INVALID_PARAMETER_OPERATION;
    }
    *value = 0;
    return StatusCode::NOT_IMPLEMENTED;
}

std::string remidy::PluginInstanceAUv2::ParameterSupport::valueToString(uint32_t index, double value) {
    auto& enums = parameter_list[index]->enums();

    // For enumerated/indexed parameters, search for matching value
    if (!enums.empty()) {
        for (const auto& e : enums) {
            if (std::abs(e.value - value) < 0.0001) {
                return e.label;
            }
        }
        // If no exact match found, return first/last based on proximity
        if (value <= enums.front().value)
            return enums.front().label;
        if (value >= enums.back().value)
            return enums.back().label;
    }

    // For continuous parameters, try to get formatted string from AU
    AudioUnitParameterID paramId = au_param_id_list[index];
    UInt32 dataSize = sizeof(AudioUnitParameterValueName);

    Float32 floatValue = (Float32)value;
    AudioUnitParameterValueName valueName;
    valueName.inParamID = paramId;
    valueName.inValue = &floatValue;
    valueName.outName = nullptr;

    OSStatus result = AudioUnitGetProperty(owner->instance,
                                          kAudioUnitProperty_ParameterValueName,
                                          kAudioUnitScope_Global,
                                          paramId,
                                          &valueName,
                                          &dataSize);

    if (result == noErr && valueName.outName != nullptr) {
        char buffer[256];
        CFStringGetCString(valueName.outName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
        CFRelease(valueName.outName);
        return std::string(buffer);
    }

    // Fallback: format as number
    return std::format("{:.3f}", value);
}

std::string remidy::PluginInstanceAUv2::ParameterSupport::valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) {
    (void) context;
    return valueToString(index, value);
}

void remidy::PluginInstanceAUv2::ParameterSupport::refreshParameterMetadata(uint32_t index) {
    if (index >= parameter_list.size())
        return;

    auto id = au_param_id_list[index];
    AudioUnitParameterInfo info;
    UInt32 size = sizeof(info);
    auto result = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterInfo,
                                       kAudioUnitScope_Global, id, &info, &size);
    if (result == noErr) {
        parameter_list[index]->updateRange(info.minValue, info.maxValue, info.defaultValue);
    }
}

void remidy::PluginInstanceAUv2::ParameterSupport::installParameterListener() {
    if (parameter_listener)
        return;

    if (AUEventListenerCreate(parameterEventCallback, this, CFRunLoopGetMain(), kCFRunLoopDefaultMode, 0.1f, 0.1f, &parameter_listener) != noErr)
        parameter_listener = nullptr;

    if (!parameter_listener)
        return;

    size_t parameterCount = au_param_id_list_size / sizeof(AudioUnitParameterID);
    for (size_t i = 0; i < parameterCount; ++i) {
        AudioUnitEvent event{};
        event.mEventType = kAudioUnitEvent_ParameterValueChange;
        event.mArgument.mParameter.mAudioUnit = owner->instance;
        event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
        event.mArgument.mParameter.mElement = 0;
        event.mArgument.mParameter.mParameterID = au_param_id_list[i];
        AUEventListenerAddEventType(parameter_listener, this, &event);
    }

    AudioUnitEvent presetEvent{};
    presetEvent.mEventType = kAudioUnitEvent_PropertyChange;
    presetEvent.mArgument.mProperty.mAudioUnit = owner->instance;
    presetEvent.mArgument.mProperty.mScope = kAudioUnitScope_Global;
    presetEvent.mArgument.mProperty.mElement = 0;
    presetEvent.mArgument.mProperty.mPropertyID = kAudioUnitProperty_PresentPreset;
    AUEventListenerAddEventType(parameter_listener, this, &presetEvent);
}

void remidy::PluginInstanceAUv2::ParameterSupport::uninstallParameterListener() {
    if (!parameter_listener)
        return;
    AUListenerDispose(parameter_listener);
    parameter_listener = nullptr;
}

void remidy::PluginInstanceAUv2::ParameterSupport::parameterEventCallback(void* refCon, void* object, const AudioUnitEvent* event, UInt64 hostTime, Float32 value) {
    (void)object;
    (void)hostTime;
    auto* support = static_cast<ParameterSupport*>(refCon);
    if (support)
        support->handleParameterEvent(event, value);
}

void remidy::PluginInstanceAUv2::ParameterSupport::handleParameterEvent(const AudioUnitEvent* event, Float32 value) {
    if (!event)
        return;

    if (event->mEventType == kAudioUnitEvent_ParameterValueChange) {
        auto index = indexForParameterId(event->mArgument.mParameter.mParameterID);
        if (!index.has_value())
            return;
        notifyParameterChangeListeners(index.value(), static_cast<double>(value));
        return;
    }

    if (event->mEventType == kAudioUnitEvent_PropertyChange &&
        event->mArgument.mProperty.mPropertyID == kAudioUnitProperty_PresentPreset) {
        refreshAllParameterMetadata();
        for (size_t i = 0; i < parameter_list.size(); ++i) {
            double currentValue;
            if (getParameter(static_cast<uint32_t>(i), &currentValue) == StatusCode::OK)
                notifyParameterChangeListeners(static_cast<uint32_t>(i), currentValue);
        }
    }
}

std::optional<uint32_t> remidy::PluginInstanceAUv2::ParameterSupport::indexForParameterId(AudioUnitParameterID id) const {
    auto it = parameter_id_to_index.find(id);
    if (it == parameter_id_to_index.end())
        return std::nullopt;
    return it->second;
}

#endif
