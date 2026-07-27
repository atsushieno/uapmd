// Provides the miniaudio implementation for platforms that do not compile
// MiniAudioIODevice.cpp (which normally carries #define MINIAUDIO_IMPLEMENTATION).
// Required on Emscripten and Android so that uapmd-data's audio file decoder
// (MiniAudioFileFactory) can resolve the ma_decoder_* symbols at link time.
//
// MA_NO_DEVICE_IO disables the audio device API (playback/capture) which is
// handled by platform-specific backends on these platforms.

#define MA_NO_DEVICE_IO
#define MINIAUDIO_IMPLEMENTATION 1
#include <miniaudio.h>
