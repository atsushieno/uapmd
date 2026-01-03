#if __APPLE__

#include <format>
#include <algorithm>
#include <unordered_map>
#import <AudioToolbox/AUAudioUnit.h>
#include "PluginFormatAUv3.hpp"
#include "AUv2Helper.hpp"

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

remidy::PluginInstanceAUv3::ParameterSupport::ParameterSupport(remidy::PluginInstanceAUv3 *owner)
        : owner(owner) {
    auto impl = [&] {
        @autoreleasepool {
            if (owner->audioUnit == nil) {
                owner->logger()->logError("%s: ParameterSupport - audioUnit is nil", owner->name.c_str());
                return;
            }

            AUParameterTree* parameterTree = [owner->audioUnit parameterTree];
            if (parameterTree == nil) {
                owner->logger()->logWarning("%s: ParameterSupport - no parameter tree", owner->name.c_str());
                return;
            }

            NSArray<AUParameter*>* allParameters = [parameterTree allParameters];
            parameter_list.reserve([allParameters count]);

            for (NSUInteger i = 0; i < [allParameters count]; i++) {
                AUParameter* param = allParameters[i];

                std::string idString = std::format("{}", [param address]);
                std::string pName = std::string([[param displayName] UTF8String]);
                std::string path = keyPathToGroupPath([param keyPath]);

                // Get enumeration values if available
                // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                std::vector<ParameterEnumeration> enums;
                NSArray<NSString*>* valueStrings = [param valueStrings];
                if (valueStrings != nil && [valueStrings count] > 0) {
                    enums.reserve([valueStrings count]);
                    for (NSUInteger e = 0; e < [valueStrings count]; e++) {
                        NSString* label = valueStrings[e];
                        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                        // ParameterEnumeration constructor takes (label, value) as references
                        std::string labelStr = std::string([label UTF8String]);
                        double enumValue = static_cast<double>(e);
                        enums.emplace_back(labelStr, enumValue);
                    }
                }

                // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                // AUParameter defaultValue property doesn't exist, use value instead
                auto p = new PluginParameter(
                    static_cast<uint32_t>(i),
                    idString,
                    pName,
                    path,
                    static_cast<double>([param value]), // Use current value as default
                    [param minValue],
                    [param maxValue],
                    ![param implementorValueObserver], // automatable if has observer
                    true, // readable
                    false, // not hidden
                    [param unit] == kAudioUnitParameterUnit_Indexed, // NOTE: correct constant name
                    enums
                );
                parameter_list.emplace_back(p);
            }
        }
    };

    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::Parameters)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();

    installParameterObserver();
}

remidy::PluginInstanceAUv3::ParameterSupport::~ParameterSupport() {
    uninstallParameterObserver();
    for (auto* param : parameter_list) {
        delete param;
    }
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceAUv3::ParameterSupport::parameters() {
    return parameter_list;
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceAUv3::ParameterSupport::perNoteControllers(
    PerNoteControllerContextTypes types, PerNoteControllerContext context) {
    // AUv3 doesn't have built-in per-note controller support
    // Return empty list for now
    static std::vector<PluginParameter*> empty;
    return empty;
}

remidy::StatusCode remidy::PluginInstanceAUv3::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    @autoreleasepool {
        if (owner->audioUnit == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

        if (index >= parameter_list.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

        NSArray<AUParameter*>* allParameters = [tree allParameters];
        if (index >= [allParameters count])
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameter* param = allParameters[index];
        [param setValue:static_cast<AUValue>(value)];

        return StatusCode::OK;
    }
}

remidy::StatusCode remidy::PluginInstanceAUv3::ParameterSupport::getParameter(uint32_t index, double* value) {
    @autoreleasepool {
        if (!value) {
            owner->logger()->logError("ParameterSupport::getParameter(): value is null");
            return StatusCode::INVALID_PARAMETER_OPERATION;
        }

        if (owner->audioUnit == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

        if (index >= parameter_list.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

        NSArray<AUParameter*>* allParameters = [tree allParameters];
        if (index >= [allParameters count])
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameter* param = allParameters[index];
        *value = static_cast<double>([param value]);

        return StatusCode::OK;
    }
}

remidy::StatusCode remidy::PluginInstanceAUv3::ParameterSupport::setPerNoteController(
    PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
    owner->logger()->logInfo("remidy::PluginInstanceAUv3::setPerNoteController not implemented");
    return StatusCode::NOT_IMPLEMENTED;
}

remidy::StatusCode remidy::PluginInstanceAUv3::ParameterSupport::getPerNoteController(
    PerNoteControllerContext context, uint32_t index, double* value) {
    owner->logger()->logInfo("remidy::PluginInstanceAUv3::getPerNoteController not implemented");
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    *value = 0;
    return StatusCode::NOT_IMPLEMENTED;
}

std::string remidy::PluginInstanceAUv3::ParameterSupport::valueToString(uint32_t index, double value) {
    @autoreleasepool {
        if (owner->audioUnit == nil || index >= parameter_list.size())
            return std::format("{:.3f}", value);

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return std::format("{:.3f}", value);

        NSArray<AUParameter*>* allParameters = [tree allParameters];
        if (index >= [allParameters count])
            return std::format("{:.3f}", value);

        AUParameter* param = allParameters[index];
        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AUValue auValue = static_cast<AUValue>(value);
        NSString* str = [param stringFromValue:&auValue];
        if (str != nil)
            return std::string([str UTF8String]);

        return std::format("{:.3f}", value);
    }
}

std::string remidy::PluginInstanceAUv3::ParameterSupport::valueToStringPerNote(
    PerNoteControllerContext context, uint32_t index, double value) {
    return valueToString(index, value);
}

void remidy::PluginInstanceAUv3::ParameterSupport::refreshParameterMetadata(uint32_t index) {
    @autoreleasepool {
        if (owner->audioUnit == nil || index >= parameter_list.size())
            return;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return;

        NSArray<AUParameter*>* allParameters = [tree allParameters];
        if (index >= [allParameters count])
            return;

        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AUParameter* param = allParameters[index];
        // AUParameter doesn't have defaultValue, use current value
        parameter_list[index]->updateRange([param minValue], [param maxValue], static_cast<double>([param value]));
    }
}

void remidy::PluginInstanceAUv3::ParameterSupport::installParameterObserver() {
    @autoreleasepool {
        if (parameterObserverToken != nil)
            return;

        if (owner->audioUnit == nil)
            return;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return;

        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        // __weak doesn't work with C++ pointers, using __unsafe_unretained instead
        // This is safe because the observer is uninstalled in the destructor
        __unsafe_unretained PluginInstanceAUv3::ParameterSupport* weakSelf = this;
        AUParameterObserverToken token = [tree tokenByAddingParameterObserver:^(AUParameterAddress address, AUValue value) {
            PluginInstanceAUv3::ParameterSupport* strongSelf = weakSelf;
            if (!strongSelf)
                return;

            // Find parameter index by address
            for (size_t i = 0; i < strongSelf->parameter_list.size(); i++) {
                // Parameter ID is stored as string, need to convert back
                if (std::stoul(strongSelf->parameter_list[i]->stableId()) == address) {
                    strongSelf->notifyParameterChangeListeners(static_cast<uint32_t>(i), static_cast<double>(value));
                    break;
                }
            }
        }];

        // Store the token - no need for bridge casts in non-ARC
        parameterObserverToken = token;
    }
}

void remidy::PluginInstanceAUv3::ParameterSupport::uninstallParameterObserver() {
    @autoreleasepool {
        if (parameterObserverToken == nil)
            return;

        if (owner->audioUnit != nil) {
            AUParameterTree* tree = [owner->audioUnit parameterTree];
            if (tree != nil) {
                // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                [tree removeParameterObserver:parameterObserverToken];
            }
        }

        parameterObserverToken = nil;
    }
}

#endif
