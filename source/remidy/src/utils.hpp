#pragma once

#include <string>
#include <functional>
#include <filesystem>

#if __APPLE__
// Forward declare CoreFoundation type to avoid including the header
typedef const struct __CFString* CFStringRef;
CFStringRef createCFString(const char* s);
#endif

// string-to-and-from-hex converters
std::string hexBinaryToString(const char* s, const size_t size, const bool capital = false);
std::string stringToHexBinary(std::string s);

void* loadLibraryFromBinary(std::filesystem::path& vst3Dir);
