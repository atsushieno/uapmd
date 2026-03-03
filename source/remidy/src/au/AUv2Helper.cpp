#if __APPLE__

// AUv2Helper.cpp: macOS-only AUv2 utilities.
// cfStringToString is inline in AUv2Helper.hpp (also needed by iOS AUv3 code).
// audioUnitHasIO and retrieveCFStringRelease are AUv2-only; this file is
// excluded on iOS via the `if(NOT IOS)` CMake guard.

#include "AUv2Helper.hpp"

bool audioUnitHasIO(AudioUnit audioUnit, AudioUnitScope scope) {
    UInt32 count = 0;
    UInt32 size = sizeof(count);

    auto status = AudioUnitGetProperty(audioUnit, kAudioUnitProperty_ElementCount, scope, 0, &count, &size);
    if (status)
        return false;

    for (UInt32 busIndex = 0; busIndex < count; busIndex++) {
        AudioStreamBasicDescription desc;
        size = sizeof(desc);
        status = AudioUnitGetProperty(audioUnit, kAudioUnitProperty_StreamFormat, scope, busIndex, &desc, &size);
        if (status && desc.mChannelsPerFrame > 0)
            return true;
    }
    return false;
}

std::string retrieveCFStringRelease(const std::function<void(CFStringRef&)>&& retriever) {
    CFStringRef cf{nullptr};
    retriever(cf);
    if (!cf)
        return {};

    auto ret = cfStringToString(cf);
    CFRelease(cf);
    return ret;
}

#endif
