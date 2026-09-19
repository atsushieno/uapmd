#pragma once

#include <string>
#include <filesystem>

#if __APPLE__
// macOS modules own both a dlopen handle and the bundle used for VST3 initialization.
typedef struct __CFBundle* CFBundleRef;
CFBundleRef getLibraryBundle(void* module);
void* getLibrarySymbol(void* module, const char* name);
void unloadLibrary(void* module);
#endif

// string-to-and-from-hex converters
std::string hexBinaryToString(const char* s, const size_t size, const bool capital = false);
std::string stringToHexBinary(std::string s);
// VST3 TUID (GUID-like) helpers
std::string vst3TuidToString(const char* s, const size_t size, const bool capital = true);
std::string stringToVst3Tuid(std::string s);

void* loadLibraryFromBinary(std::filesystem::path& pluginDirOrFile);
