#include "PluginFormatAU.hpp"

std::vector<uint8_t> remidy::PluginInstanceAU::PluginStatesAU::getState(remidy::PluginStateSupport::StateContextType stateContextType,
                                                                        bool includeUiState) {
    CFPropertyListRef dict;
    UInt32 size;
    AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &dict, &size);
    CFErrorRef error;
    auto data = CFPropertyListCreateData(nullptr, dict, kCFPropertyListBinaryFormat_v1_0, 0, &error);
    size = CFDataGetLength(data);
    std::vector<uint8_t> ret{};
    ret.resize(size);
    memcpy(ret.data(), CFDataGetBytePtr(data), size);
    CFRelease(dict);
    return ret;
}

void remidy::PluginInstanceAU::PluginStatesAU::setState(std::vector<uint8_t> &state,
                                                        remidy::PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
    CFReadStreamRef stream = CFReadStreamCreateWithBytesNoCopy(nullptr, state.data(), state.size(), nullptr);
    CFErrorRef error;
    CFPropertyListFormat propertyListFormat[]{kCFPropertyListBinaryFormat_v1_0};
    CFPropertyListRef dict = CFPropertyListCreateWithStream(nullptr, stream, state.size(), 0, propertyListFormat, &error);
    if (dict) {
        // FIXME: error reporting maybe?
        auto status = AudioUnitSetProperty(owner->instance, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, dict, state.size());
        CFRelease(dict);
        CFReadStreamClose(stream);
    } else {
        std::cerr << "PluginStatesAU::setState(): failed to create property list" << std::endl;
    }
}
