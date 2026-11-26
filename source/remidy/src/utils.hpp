#pragma once

#include <cstdint>
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
// VST3 note ID for a UMP note, so that note expressions can address the note they apply to.
inline int32_t vst3NoteId(uint8_t channel, uint8_t note) { return static_cast<int32_t>(channel) * 128 + note; }

void* loadLibraryFromBinary(std::filesystem::path& pluginDirOrFile);

// Semitone offset of a MIDI 2.0 Pitch 7.9 note attribute from the note number it is attached to.
inline double pitch7_9OffsetSemitones(uint8_t note, uint16_t attribute) {
    return static_cast<double>(attribute) / 512.0 - static_cast<double>(note);
}

// MIDI 2.0 per-note pitch bend in semitones, read with a +/-2 semitone sensitivity (as NoteClipWriter writes it).
inline double perNotePitchBendSemitones(uint32_t data) {
    return (static_cast<double>(data) - 2147483648.0) / 2147483648.0 * 2.0;
}
