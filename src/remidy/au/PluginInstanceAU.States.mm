#include "PluginFormatAU.hpp"

std::vector<uint8_t> remidy::PluginInstanceAU::PluginStatesAU::getState(remidy::PluginStateSupport::StateContextType stateContextType,
                                                                        bool includeUiState) {
    std::vector<uint8_t> ret{};
    auto impl = [&] {

    CFPropertyListRef dict{nullptr};
    UInt32 size = sizeof(dict);
    auto status = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &dict, &size);
    if (status != noErr) {
        Logger::global()->logError("PluginStatesAU::getState(): failed to get state: %d", status);
        return;
    }
    CFErrorRef error{};
    auto data = CFPropertyListCreateData(nullptr, dict, kCFPropertyListBinaryFormat_v1_0, 0, &error);
    if (!data) {
        Logger::global()->logError("PluginStatesAU::getState(): failed to create property list data");
        if (error)
            CFRelease(error);
        CFRelease(dict);
        return;
    }
    size = CFDataGetLength(data);
    if (size > 0) {
        ret.resize(size);
        memcpy(ret.data(), CFDataGetBytePtr(data), size);
    } else {
        ret.clear();
    }
    CFRelease(data);
    CFRelease(dict);
    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::State)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();
    return ret;
}

void remidy::PluginInstanceAU::PluginStatesAU::setState(std::vector<uint8_t> &state,
                                                        remidy::PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
    auto impl = [&] {
    CFReadStreamRef stream = CFReadStreamCreateWithBytesNoCopy(nullptr, state.data(), state.size(), kCFAllocatorNull);
    if (!stream) {
        Logger::global()->logError("PluginStatesAU::setState(): failed to create read stream");
        return;
    }
    if (!CFReadStreamOpen(stream)) {
        Logger::global()->logError("PluginStatesAU::setState(): failed to open read stream");
        CFRelease(stream);
        return;
    }
    CFErrorRef error{};
    CFPropertyListFormat propertyListFormat[]{kCFPropertyListBinaryFormat_v1_0};
    CFPropertyListRef dict = CFPropertyListCreateWithStream(nullptr, stream, state.size(), 0, propertyListFormat, &error);
    if (dict) {
        auto status = AudioUnitSetProperty(owner->instance, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &dict, sizeof(dict));
        if (status != noErr)
            Logger::global()->logError("PluginStatesAU::setState(): failed to set state: %d", status);
        CFRelease(dict);
    } else {
        Logger::global()->logError("PluginStatesAU::setState(): failed to create property list");
    }
    CFReadStreamClose(stream);
    CFRelease(stream);
    if (error)
        CFRelease(error);
    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::State)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();
}
