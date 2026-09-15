#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// A C++ port of DrumScript's deterministic drum classifier (Apache-2.0; see
// NOTICE beside this header).
//
// The classifier takes one onset at a time, measures six physical properties of
// the audio around it, and decides which drums were struck by comparing those
// against tuned thresholds. There is no model to load: the thresholds are the
// model, which is why this backend -- unlike Basic Pitch -- downloads nothing at
// configure time and embeds nothing in the binary.
namespace uapmd_drumscript {

// The kit the classifier can name, plus the General MIDI percussion note each
// maps to. Taken from DrumScript's DRUM_NOTATION_MAP.
enum class DrumInstrument {
    Kick,
    Snare,
    LowTom,
    MidTom,
    HighTom,
    HiHatClosed,
    HiHatOpen,
    Crash,
    Ride,
    Unknown,
};

// General MIDI percussion note number, or -1 for Unknown.
int generalMidiNote(DrumInstrument instrument) noexcept;
std::string_view instrumentName(DrumInstrument instrument) noexcept;

// What the classifier measures around one onset. Two slices are needed because
// the spectral properties want a short window for purity while decay needs long
// enough to hear a cymbal ring out.
struct DrumPhysics {
    // Strongest bin of the mean magnitude spectrum, in Hz.
    double peak_freq{};
    // Spectral centre of mass, in Hz: brightness.
    double centroid{};
    // Seconds from the loudest frame until the signal falls 20 dB below it.
    double decay{};
    // Share of spectral energy below 150 Hz. High for a kick.
    double lfer{};
    // Share above 2 kHz. This is what snare wires sound like.
    double hfer{};
    // Share above 5 kHz. This is what metal sounds like.
    double hfer_5k{};
};

// Every threshold the decision rules read, so they can be re-derived for
// material they were not tuned on without editing the rules themselves.
// Defaults are DrumScript's, measured against its own sample set.
struct DrumThresholds {
    double kick_lfer_min{0.32};
    double kick_freq_min{40.0};
    double kick_freq_max{140.0};

    double snare_freq_min{120.0};
    double snare_freq_max{450.0};
    double snare_hfer_min{0.15};
    // Above this the event is broadband noise rather than wires.
    double snare_hfer_max{0.85};

    double tom_min_decay{0.28};
    double tom_freq_low_max{92.0};
    double tom_freq_mid_max{120.0};
    double tom_freq_high_max{400.0};

    double idiophone_min_hfer_5k{0.15};
    double hat_closed_max_decay{0.30};
    double hat_open_max_decay{0.60};
    // Brighter than this and a long metal decay is a crash, not a ride.
    double crash_min_centroid{5500.0};
};

// Analysis window geometry. The thresholds above were tuned against features
// computed with exactly these, so changing one invalidates the other.
inline constexpr int kFftSize = 2048;
inline constexpr int kSpectrumHopSamples = 128;
// librosa's rms() default, which DrumScript relies on by not passing one.
inline constexpr int kRmsHopSamples = 512;
inline constexpr double kShortSliceSeconds = 0.200;
inline constexpr double kLongSliceSeconds = 1.500;

// Measures one onset. `short_slice` drives the spectral features and
// `long_slice` the decay; both start at the onset.
DrumPhysics measure(std::span<const float> short_slice,
                    std::span<const float> long_slice,
                    double sample_rate);

// Names every drum the measurements are consistent with, because a kick and a
// hi-hat struck together have to come out as both rather than as whichever
// scored higher. Returns { Unknown } when no rule matches.
std::vector<DrumInstrument> classify(const DrumPhysics& physics,
                                     const DrumThresholds& thresholds = {});

} // namespace uapmd_drumscript
