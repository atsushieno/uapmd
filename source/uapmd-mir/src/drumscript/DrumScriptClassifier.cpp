#include "DrumScriptClassifier.hpp"

#include <algorithm>
#include <cmath>

#include <librosa/core/spectrum.hpp>
#include <librosa/feature/spectral.hpp>

namespace uapmd_drumscript {

namespace {

librosa::ArrayXr toArray(std::span<const float> samples) {
    librosa::ArrayXr out(static_cast<Eigen::Index>(samples.size()));
    for (size_t i = 0; i < samples.size(); ++i)
        out[static_cast<Eigen::Index>(i)] = static_cast<librosa::Real>(samples[i]);
    return out;
}

// Share of the spectrum's energy above (or below) a cutoff. The rules are all
// phrased as ratios so that they hold at any input gain.
double energyRatio(const librosa::ArrayXr& spectrum,
                   const librosa::ArrayXr& frequencies,
                   double total,
                   double cutoffHz,
                   bool above) {
    if (total <= 0.0)
        return 0.0;
    double sum = 0.0;
    for (Eigen::Index i = 0; i < spectrum.size(); ++i) {
        const double hz = frequencies[i];
        if (above ? (hz > cutoffHz) : (hz <= cutoffHz))
            sum += spectrum[i];
    }
    return sum / total;
}

} // namespace

int generalMidiNote(DrumInstrument instrument) noexcept {
    switch (instrument) {
        case DrumInstrument::Kick: return 36;
        case DrumInstrument::Snare: return 38;
        case DrumInstrument::LowTom: return 45;
        case DrumInstrument::MidTom: return 47;
        case DrumInstrument::HighTom: return 48;
        case DrumInstrument::HiHatClosed: return 42;
        case DrumInstrument::HiHatOpen: return 46;
        case DrumInstrument::Crash: return 49;
        case DrumInstrument::Ride: return 51;
        case DrumInstrument::Unknown: break;
    }
    return -1;
}

std::string_view instrumentName(DrumInstrument instrument) noexcept {
    switch (instrument) {
        case DrumInstrument::Kick: return "kick";
        case DrumInstrument::Snare: return "snare";
        case DrumInstrument::LowTom: return "low_tom";
        case DrumInstrument::MidTom: return "mid_tom";
        case DrumInstrument::HighTom: return "high_tom";
        case DrumInstrument::HiHatClosed: return "hi_hat_closed";
        case DrumInstrument::HiHatOpen: return "hi_hat_open";
        case DrumInstrument::Crash: return "crash";
        case DrumInstrument::Ride: return "ride";
        case DrumInstrument::Unknown: break;
    }
    return "unknown";
}

DrumPhysics measure(std::span<const float> short_slice,
                    std::span<const float> long_slice,
                    double sample_rate) {
    DrumPhysics physics;
    if (short_slice.empty() || sample_rate <= 0.0)
        return physics;

    const auto shortAudio = toArray(short_slice);
    const auto sr = static_cast<librosa::Real>(sample_rate);

    // Magnitude spectrogram, averaged across frames into one spectrum.
    const librosa::ArrayXXr magnitude =
        librosa::stft(shortAudio, kFftSize, kSpectrumHopSamples).abs();
    if (magnitude.size() == 0)
        return physics;
    const librosa::ArrayXr spectrum = magnitude.rowwise().mean();

    librosa::ArrayXr frequencies(spectrum.size());
    for (Eigen::Index i = 0; i < spectrum.size(); ++i)
        frequencies[i] = static_cast<librosa::Real>(i) * sr / kFftSize;

    Eigen::Index peakIndex = 0;
    spectrum.maxCoeff(&peakIndex);
    physics.peak_freq = frequencies[peakIndex];

    const librosa::ArrayXXr centroid =
        librosa::feature::spectral_centroid(magnitude, sr, kFftSize);
    physics.centroid = centroid.size() > 0 ? centroid.mean() : 0.0;

    // Decay is measured on the long slice, and -- as in DrumScript -- with
    // librosa's default 512-sample rms hop rather than the 128 used for the
    // spectrum above. Every decay threshold is calibrated against that hop.
    if (!long_slice.empty()) {
        const librosa::ArrayXXr rmsFrames =
            librosa::feature::rms(toArray(long_slice), kFftSize, kRmsHopSamples);
        if (rmsFrames.size() > 0) {
            const librosa::ArrayXr rms = rmsFrames.row(0);
            Eigen::Index peakRmsIndex = 0;
            const double peakRms = rms.maxCoeff(&peakRmsIndex);
            const double threshold = peakRms * 0.1; // -20 dB
            Eigen::Index decayFrames = 0;
            for (Eigen::Index i = peakRmsIndex; i < rms.size(); ++i) {
                if (rms[i] < threshold)
                    break;
                ++decayFrames;
            }
            physics.decay = static_cast<double>(decayFrames) * kRmsHopSamples / sample_rate;
        }
    }

    const double total = spectrum.sum();
    physics.lfer = energyRatio(spectrum, frequencies, total, 150.0, false);
    physics.hfer = energyRatio(spectrum, frequencies, total, 2000.0, true);
    physics.hfer_5k = energyRatio(spectrum, frequencies, total, 5000.0, true);
    return physics;
}

namespace {

// Skins: kick, snare and toms, separated by where their energy sits and how
// long they ring.
void classifyMembranophone(const DrumPhysics& p,
                           const DrumThresholds& t,
                           std::vector<DrumInstrument>& out) {
    if (p.lfer >= t.kick_lfer_min && p.peak_freq >= t.kick_freq_min && p.peak_freq <= t.kick_freq_max) {
        // A kick is a short thud; a tom in the same register rings on.
        const bool isPureTom = p.hfer < t.snare_hfer_min && p.decay >= t.tom_min_decay;
        if (!isPureTom)
            out.push_back(DrumInstrument::Kick);
    }

    const bool isSnareFreq = p.peak_freq >= t.snare_freq_min && p.peak_freq <= t.snare_freq_max;
    const bool hasSnareWire = p.hfer >= t.snare_hfer_min && p.hfer < t.snare_hfer_max;
    if (hasSnareWire && isSnareFreq)
        out.push_back(DrumInstrument::Snare);

    const bool isPure = p.hfer < t.snare_hfer_min;
    const bool isResonant = p.decay >= t.tom_min_decay;
    if (isPure && isResonant) {
        if (p.peak_freq <= t.tom_freq_low_max) {
            // A low tom and a kick occupy the same band; the kick rule already
            // claimed this onset if it was short enough to be one.
            if (std::find(out.begin(), out.end(), DrumInstrument::Kick) == out.end())
                out.push_back(DrumInstrument::LowTom);
        }
        else if (p.peak_freq <= t.tom_freq_mid_max)
            out.push_back(DrumInstrument::MidTom);
        else if (p.peak_freq <= t.tom_freq_high_max)
            out.push_back(DrumInstrument::HighTom);
    }
}

// Metals: hats and cymbals, separated purely by how long they ring, then by
// brightness once they ring longer than a hat can.
void classifyIdiophone(const DrumPhysics& p,
                       const DrumThresholds& t,
                       std::vector<DrumInstrument>& out) {
    if (p.hfer_5k < t.idiophone_min_hfer_5k)
        return;
    if (p.decay <= t.hat_closed_max_decay)
        out.push_back(DrumInstrument::HiHatClosed);
    else if (p.decay <= t.hat_open_max_decay)
        out.push_back(DrumInstrument::HiHatOpen);
    else if (p.centroid > t.crash_min_centroid)
        out.push_back(DrumInstrument::Crash);
    else
        out.push_back(DrumInstrument::Ride);
}

} // namespace

std::vector<DrumInstrument> classify(const DrumPhysics& physics, const DrumThresholds& thresholds) {
    std::vector<DrumInstrument> instruments;
    classifyMembranophone(physics, thresholds, instruments);
    classifyIdiophone(physics, thresholds, instruments);
    if (instruments.empty())
        instruments.push_back(DrumInstrument::Unknown);
    return instruments;
}

} // namespace uapmd_drumscript
