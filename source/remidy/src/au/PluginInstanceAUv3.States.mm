#if __APPLE__

#include <vector>
#include "PluginFormatAU.hpp"

std::vector<uint8_t> remidy::PluginInstanceAUv3::PluginStatesAUv3::getState(StateContextType stateContextType, bool includeUiState) {
    @autoreleasepool {
        if (owner->audioUnit == nil) {
            owner->logger()->logError("%s: getState - audioUnit is nil", owner->name.c_str());
            return {};
        }

        NSError* error = nil;
        NSDictionary* state = nil;

        // AUv3 provides fullState (includes UI) and fullStateForDocument (component only)
        if (includeUiState) {
            state = [owner->audioUnit fullState];
        } else {
            state = [owner->audioUnit fullStateForDocument];
        }

        if (state == nil) {
            owner->logger()->logWarning("%s: getState returned nil state", owner->name.c_str());
            return {};
        }

        // Serialize to NSData
        NSData* data = [NSPropertyListSerialization dataWithPropertyList:state
                                                                   format:NSPropertyListBinaryFormat_v1_0
                                                                  options:0
                                                                    error:&error];

        if (error != nil) {
            owner->logger()->logError("%s: Failed to serialize state: %s",
                                    owner->name.c_str(),
                                    [[error localizedDescription] UTF8String]);
            return {};
        }

        if (data == nil) {
            return {};
        }

        // Convert NSData to vector<uint8_t>
        std::vector<uint8_t> result;
        result.resize([data length]);
        [data getBytes:result.data() length:[data length]];

        return result;
    }
}

void remidy::PluginInstanceAUv3::PluginStatesAUv3::setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) {
    @autoreleasepool {
        if (owner->audioUnit == nil) {
            owner->logger()->logError("%s: setState - audioUnit is nil", owner->name.c_str());
            return;
        }

        if (state.empty()) {
            owner->logger()->logWarning("%s: setState called with empty state", owner->name.c_str());
            return;
        }

        // Convert vector<uint8_t> to NSData
        NSData* data = [NSData dataWithBytes:state.data() length:state.size()];

        NSError* error = nil;
        NSDictionary* stateDict = [NSPropertyListSerialization propertyListWithData:data
                                                                             options:NSPropertyListImmutable
                                                                              format:nil
                                                                               error:&error];

        if (error != nil) {
            owner->logger()->logError("%s: Failed to deserialize state: %s",
                                    owner->name.c_str(),
                                    [[error localizedDescription] UTF8String]);
            return;
        }

        if (stateDict == nil || ![stateDict isKindOfClass:[NSDictionary class]]) {
            owner->logger()->logError("%s: setState - deserialized data is not a dictionary", owner->name.c_str());
            return;
        }

        // Set state on the audio unit
        if (includeUiState) {
            [owner->audioUnit setFullState:stateDict];
        } else {
            [owner->audioUnit setFullStateForDocument:stateDict];
        }
    }
}

#endif
