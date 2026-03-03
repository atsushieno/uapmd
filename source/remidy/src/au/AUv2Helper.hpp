#include <AudioToolbox/AudioToolbox.h>
#include <functional>
#include <string>
#include <cstring>

// cfStringToString is inline: it uses only CoreFoundation (available on both
// macOS and iOS) and must link on iOS where AUv2Helper.cpp is excluded.
inline std::string cfStringToString(CFStringRef s) {
    if (!s)
        return {};
    auto length = CFStringGetLength(s);
    if (length == 0)
        return {};
    auto maxSize = static_cast<size_t>(
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1);
    std::string buffer(maxSize, '\0');
    if (!CFStringGetCString(s, buffer.data(), static_cast<CFIndex>(maxSize), kCFStringEncodingUTF8))
        return {};
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
}

// audioUnitHasIO and retrieveCFStringRelease are macOS-only (AUv2); their
// implementations live in AUv2Helper.cpp which is excluded on iOS.
bool audioUnitHasIO(AudioUnit audioUnit, AudioUnitScope scope);

std::string retrieveCFStringRelease(const std::function<void(CFStringRef&)>&& retriever);
