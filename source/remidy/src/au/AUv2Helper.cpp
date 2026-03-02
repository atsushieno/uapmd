#if __APPLE__

#include <AudioToolbox/AudioToolbox.h>
#include <string>
#include <functional>
#include <cstring>

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

std::string cfStringToString(CFStringRef s) {
    if (!s)
        return {};

    auto length = CFStringGetLength(s);
    if (length == 0)
        return {};

    auto maxSize = static_cast<size_t>(CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1);
    std::string buffer(maxSize, '\0');
    if (!CFStringGetCString(s, buffer.data(), maxSize, kCFStringEncodingUTF8))
        return {};

    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
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
