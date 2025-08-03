#include "PluginFormatAU.hpp"

void remidy::AudioPluginInstanceAU::PluginStatesAU::getState(std::vector<uint8_t> &state,
                                                             remidy::PluginStateSupport::StateContextType stateContextType,
                                                             bool includeUiState) {
    CFPropertyListRef dict;
    UInt32 size;
    AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &dict, &size);
    CFErrorRef error;
    CFWriteStreamRef stream = CFWriteStreamCreateWithBuffer(nullptr, state.data(), state.size());
    if (stream) {
        CFPropertyListWrite(dict, stream, kCFPropertyListBinaryFormat_v1_0, 0, &error);
        CFWriteStreamClose(stream);
    } else {
        // FIXME: report error
    }
    CFRelease(dict);
}

void remidy::AudioPluginInstanceAU::PluginStatesAU::setState(std::vector<uint8_t> &state,
                                                             remidy::PluginStateSupport::StateContextType stateContextType,
                                                             bool includeUiState) {
    CFReadStreamRef stream = CFReadStreamCreateWithBytesNoCopy(nullptr, state.data(), state.size(), nullptr);
    CFErrorRef error;
    CFPropertyListFormat propertyListFormat[]{kCFPropertyListBinaryFormat_v1_0};
    CFPropertyListRef dict = CFPropertyListCreateWithStream(nullptr, stream, state.size(), 0, propertyListFormat, &error);
    AudioUnitSetProperty(owner->instance, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &dict, state.size());
    CFRelease(dict);
    CFReadStreamClose(stream);
}
