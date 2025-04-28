#pragma once

#include <string>
#include <functional>
#include <filesystem>
#if WIN32
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#ifdef _MSC_VER
#define strcasecmp _wcsicmp
#endif

#if __APPLE__
CFStringRef createCFString(const char* s);
#endif

// string-to-and-from-hex converters
std::string hexBinaryToString(const char* s, const size_t size, const bool capital = false);
std::string stringToHexBinary(std::string s);

void* loadLibraryFromBinary(std::filesystem::path& vst3Dir);
