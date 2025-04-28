
#include "utils.hpp"

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

    for (size_t i = 0, c = s[0] & 0xFF; i < ret.size(); c = s[i / 2] & 0xFF) {
        ret[i++] = c > 0x9F ? (c / 16 - 9) | a : c / 16 | '0';
        ret[i++] = (c & 0xF) > 9 ? (c % 16 - 9) | a : c % 16 | '0';
    }
    return ret;
}
std::string stringToHexBinary(std::string s) {
    std::string ret{};
    ret.resize((s.size() + 1) / 2);
    for (size_t i = 0, j = 0; i < ret.size(); i++, j++) {
        ret[i] = (s[j] & '@' ? s[j] + 9 : s[j]) << 4, j++;
        ret[i] |= (s[j] & '@' ? s[j] + 9 : s[j]) & 0xF;
    }
    return ret;
}

// The returned library (platform dependent) must be released later (in the platform manner)
// It might fail due to ABI mismatch on macOS. We have to ignore the error and return nullptr.
void* loadLibraryFromBinary(std::filesystem::path& pluginDirOrFile) {
#if _WIN32
    auto ret = LoadLibraryW(pluginDirOrFile.c_str());
#elif __APPLE__
    auto cfStringRef = createCFString(pluginDirOrFile.string().c_str());
    auto ret = CFBundleCreate(kCFAllocatorDefault,
        CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
            cfStringRef,
            kCFURLPOSIXPathStyle,
            false));
#else
    auto ret = dlopen(pluginDirOrFile.c_str(), RTLD_LAZY | RTLD_LOCAL);
    //if (errno)
    //    defaultLogError("dlopen resulted in error: %s", dlerror());
#endif
    return ret;
}
