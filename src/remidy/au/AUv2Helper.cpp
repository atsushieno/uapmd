#if __APPLE__

#include <AudioToolbox/AudioToolbox.h>
#include <string>
#include <functional>

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

std::string cfStringToString(CFStringRef& s) {
    std::string ret{CFStringGetCStringPtr(s, kCFStringEncodingUTF8)};
    return ret;
}

std::string retrieveCFStringRelease(const std::function<void(CFStringRef&)>&& retriever) {
    CFStringRef cf;
    retriever(cf);
    auto ret = cfStringToString(cf);
    CFRelease(cf);
    return ret;
}

#endif
