
#include "utils.hpp"
#include <thread>
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__) && !defined(EMSCRIPTEN)
#include <dlfcn.h>
#endif

#if __APPLE__
CFStringRef createCFString(const char* s) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, (UInt8*) s, strlen(s), CFStringEncoding{}, false);
}
#endif

// These functions are mostly based on https://stackoverflow.com/a/35599923/1465645
std::string hexBinaryToString(const char* s, const size_t size, const bool capital) {
    std::string ret{};
    ret.resize(size * 2);
    const size_t a = capital ? 'A' - 1 : 'a' - 1;

    for (size_t i = 0, c = static_cast<unsigned char>(s[0]); i < ret.size(); c = static_cast<unsigned char>(s[i / 2])) {
        ret[i++] = c > 0x9F ? (c / 16 - 9) | a : c / 16 | '0';
        ret[i++] = (c & 0xF) > 9 ? (c % 16 - 9) | a : c % 16 | '0';
    }
    return ret;
}

static int hexDigitToValue(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F')
        return 10 + (ch - 'A');
    return -1;
}

std::string stringToHexBinary(std::string s) {
    std::string ret{};
    ret.reserve(s.size() / 2);

    int highNibble = -1;
    for (char ch : s) {
        int value = hexDigitToValue(ch);
        if (value < 0)
            continue;
        if (highNibble < 0) {
            highNibble = value;
            continue;
        }
        ret.push_back(static_cast<char>((highNibble << 4) | value));
        highNibble = -1;
    }
    return ret;
}

static void swapGuidByteOrder(std::array<unsigned char, 16>& bytes) {
    std::reverse(bytes.begin(), bytes.begin() + 4);
    std::reverse(bytes.begin() + 4, bytes.begin() + 6);
    std::reverse(bytes.begin() + 6, bytes.begin() + 8);
}

std::string vst3TuidToString(const char* s, const size_t size, const bool capital) {
    if (size < 16)
        return hexBinaryToString(s, size, capital);

    std::array<unsigned char, 16> bytes{};
    memcpy(bytes.data(), s, 16);
    swapGuidByteOrder(bytes);
    return hexBinaryToString(reinterpret_cast<const char*>(bytes.data()), bytes.size(), capital);
}

std::string stringToVst3Tuid(std::string s) {
    auto bytes = stringToHexBinary(std::move(s));
    if (bytes.size() < 16)
        return bytes;

    std::array<unsigned char, 16> ret{};
    memcpy(ret.data(), bytes.data(), ret.size());
    swapGuidByteOrder(ret);
    return std::string(reinterpret_cast<const char*>(ret.data()), ret.size());
}

// The returned library (platform dependent) must be released later (in the platform manner)
// It might fail due to ABI mismatch on macOS. We have to ignore the error and return nullptr.
void* loadLibraryFromBinary(std::filesystem::path& pluginDirOrFile) {
#if _WIN32
    auto ret = LoadLibraryW(pluginDirOrFile.c_str());
#elif __APPLE__
    auto cfStringRef = createCFString(pluginDirOrFile.string().c_str());
    auto cfUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
        cfStringRef,
        kCFURLPOSIXPathStyle,
        false);
    auto ret = CFBundleCreate(kCFAllocatorDefault, cfUrl);
    CFRelease(cfUrl);
    CFRelease(cfStringRef);
#elif defined(__linux__) && !defined(EMSCRIPTEN)
    auto ret = dlopen(pluginDirOrFile.c_str(), RTLD_LAZY | RTLD_LOCAL);
    //if (errno)
    //    defaultLogError("dlopen resulted in error: %s", dlerror());
#else
    (void)pluginDirOrFile;
    void* ret = nullptr;
#endif
    return ret;
}

namespace remidy {
    std::vector<std::thread::id> instance{};

    std::vector<std::thread::id>& audioThreadIds() {
        if (instance.empty())
            instance.resize(std::thread::hardware_concurrency());
        return instance;
    }
}
