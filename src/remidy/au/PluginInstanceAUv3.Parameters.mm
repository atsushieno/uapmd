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

static void* kAUParameterKVOContext = &kAUParameterKVOContext;

static NSArray<NSString*>* parameterObservationKeyPaths() {
    static NSArray<NSString*>* keyPaths = nil;
    if (!keyPaths) {
        keyPaths = [[NSArray alloc] initWithObjects:@"allParameterValues",
                                                     @"currentPreset",
                                                     nil];
    }
    return keyPaths;
}

@interface AUParameterChangeObserver : NSObject {
@public
    remidy::PluginInstanceAUv3::ParameterSupport* support;
}
@end

@implementation AUParameterChangeObserver

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    if (context == kAUParameterKVOContext) {
        if (support)
            support->handleParameterSetChange();
    } else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}

@end

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

            // Filter to only include global scope parameters (exclude per-note, per-channel, per-group)
            NSMutableArray<AUParameter*>* globalParameters = [NSMutableArray array];
            for (AUParameter* param in allParameters) {
                AUParameterAddress addr = [param address];
                AUParameter* globalParam = [parameterTree parameterWithID:addr scope:kAudioUnitScope_Global element:0];
                if (globalParam != nil) {
                    [globalParameters addObject:param];
                }
            }

            parameter_list.reserve([globalParameters count]);
            parameter_addresses.reserve([globalParameters count]);

            for (NSUInteger i = 0; i < [globalParameters count]; i++) {
                AUParameter* param = globalParameters[i];

                AUParameterAddress address = [param address];
                parameter_addresses.push_back(address);

                std::string idString = std::format("{}", address);
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

            v2AudioUnit = owner->bridgedAudioUnit;
        }
    };

    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::Parameters)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();

    installParameterObserver();
    installParameterChangeObserver();
    installV2PresetListener();
}

remidy::PluginInstanceAUv3::ParameterSupport::~ParameterSupport() {
    uninstallV2PresetListener();
    uninstallParameterChangeObserver();
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

        if (index >= parameter_list.size() || index >= parameter_addresses.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameter* param = [tree parameterWithAddress:parameter_addresses[index]];
        if (param == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

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

        if (index >= parameter_list.size() || index >= parameter_addresses.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

        AUParameter* param = [tree parameterWithAddress:parameter_addresses[index]];
        if (param == nil)
            return StatusCode::INVALID_PARAMETER_OPERATION;

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
        if (owner->audioUnit == nil || index >= parameter_list.size() || index >= parameter_addresses.size())
            return std::format("{:.3f}", value);

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return std::format("{:.3f}", value);

        AUParameter* param = [tree parameterWithAddress:parameter_addresses[index]];
        if (param == nil)
            return std::format("{:.3f}", value);

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
        if (owner->audioUnit == nil || index >= parameter_list.size() || index >= parameter_addresses.size())
            return;

        AUParameterTree* tree = [owner->audioUnit parameterTree];
        if (tree == nil)
            return;

        AUParameter* param = [tree parameterWithAddress:parameter_addresses[index]];
        if (param == nil)
            return;

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

            for (size_t i = 0; i < strongSelf->parameter_addresses.size(); i++) {
                if (strongSelf->parameter_addresses[i] == address) {
                    strongSelf->notifyParameterChangeListeners(static_cast<uint32_t>(i), static_cast<double>(value));
                    break;
                }
            }
        }];

        // Store the token - no need for bridge casts in non-ARC
        parameterObserverToken = token;
    }
}

void remidy::PluginInstanceAUv3::ParameterSupport::installParameterChangeObserver() {
    @autoreleasepool {
        if (parameterChangeObserver != nullptr)
            return;

        if (owner->audioUnit == nil)
            return;

        auto observer = [[AUParameterChangeObserver alloc] init];
        observer->support = this;
        for (NSString* keyPath in parameterObservationKeyPaths()) {
            [owner->audioUnit addObserver:observer
                               forKeyPath:keyPath
                                  options:NSKeyValueObservingOptionNew
                                  context:kAUParameterKVOContext];
        }
        parameterChangeObserver = observer;
    }
}

void remidy::PluginInstanceAUv3::ParameterSupport::uninstallParameterChangeObserver() {
    @autoreleasepool {
        if (parameterChangeObserver == nullptr)
            return;

        auto observer = static_cast<AUParameterChangeObserver*>(parameterChangeObserver);
        if (owner->audioUnit != nil) {
            for (NSString* keyPath in parameterObservationKeyPaths()) {
                @try {
                    [owner->audioUnit removeObserver:observer forKeyPath:keyPath];
                } @catch(NSException* exception) {
                    (void)exception;
                }
            }
        }
        [observer release];
        parameterChangeObserver = nullptr;
    }
}

void remidy::PluginInstanceAUv3::ParameterSupport::handleParameterSetChange() {
    refreshAllParameterMetadata();
    broadcastAllParameterValues();
}

void remidy::PluginInstanceAUv3::ParameterSupport::broadcastAllParameterValues() {
    for (size_t i = 0; i < parameter_list.size(); ++i) {
        double currentValue = 0.0;
        if (getParameter(static_cast<uint32_t>(i), &currentValue) == StatusCode::OK)
            notifyParameterChangeListeners(static_cast<uint32_t>(i), currentValue);
    }
}

void remidy::PluginInstanceAUv3::ParameterSupport::installV2PresetListener() {
    if (v2AudioUnit == nullptr || v2PresetListener)
        return;

    if (AUEventListenerCreate(v2PresetEventCallback, this, CFRunLoopGetMain(), kCFRunLoopDefaultMode, 0.1f, 0.1f, &v2PresetListener) != noErr)
        v2PresetListener = nullptr;

    if (!v2PresetListener)
        return;

    AudioUnitEvent presetEvent{};
    presetEvent.mEventType = kAudioUnitEvent_PropertyChange;
    presetEvent.mArgument.mProperty.mAudioUnit = v2AudioUnit;
    presetEvent.mArgument.mProperty.mScope = kAudioUnitScope_Global;
    presetEvent.mArgument.mProperty.mElement = 0;
    presetEvent.mArgument.mProperty.mPropertyID = kAudioUnitProperty_PresentPreset;
    AUEventListenerAddEventType(v2PresetListener, this, &presetEvent);

    AudioUnitEvent classInfoEvent{};
    classInfoEvent.mEventType = kAudioUnitEvent_PropertyChange;
    classInfoEvent.mArgument.mProperty.mAudioUnit = v2AudioUnit;
    classInfoEvent.mArgument.mProperty.mScope = kAudioUnitScope_Global;
    classInfoEvent.mArgument.mProperty.mElement = 0;
    classInfoEvent.mArgument.mProperty.mPropertyID = kAudioUnitProperty_ClassInfo;
    AUEventListenerAddEventType(v2PresetListener, this, &classInfoEvent);
}

void remidy::PluginInstanceAUv3::ParameterSupport::uninstallV2PresetListener() {
    if (!v2PresetListener)
        return;

    AUListenerDispose(v2PresetListener);
    v2PresetListener = nullptr;
}

void remidy::PluginInstanceAUv3::ParameterSupport::v2PresetEventCallback(void* refCon, void* object, const AudioUnitEvent* event, UInt64 hostTime, Float32 value) {
    (void)object;
    (void)hostTime;
    (void)value;

    if (!event || event->mEventType != kAudioUnitEvent_PropertyChange)
        return;

    auto* support = static_cast<ParameterSupport*>(refCon);
    if (!support)
        return;

    switch (event->mArgument.mProperty.mPropertyID) {
        case kAudioUnitProperty_PresentPreset:
        case kAudioUnitProperty_ClassInfo:
            support->handleParameterSetChange();
            break;
        default:
            break;
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
