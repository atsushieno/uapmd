#include "MirTempoAnalysis.hpp"
#include "MirRhythmAnalysis.hpp"
#include "MirLibrosaAnalysis.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

uint16_t read16(const char* value) {
    return static_cast<uint16_t>(static_cast<unsigned char>(value[0]))
        | static_cast<uint16_t>(static_cast<unsigned char>(value[1]) << 8);
}

uint32_t read32(const char* value) {
    return static_cast<uint32_t>(static_cast<unsigned char>(value[0]))
        | static_cast<uint32_t>(static_cast<unsigned char>(value[1]) << 8)
        | static_cast<uint32_t>(static_cast<unsigned char>(value[2]) << 16)
        | static_cast<uint32_t>(static_cast<unsigned char>(value[3]) << 24);
}

bool readFloatWave(const std::string& path, std::vector<float>& mono, int& sampleRate) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return false;

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    const char* audio = nullptr;
    size_t audioBytes = 0;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto size = static_cast<size_t>(read32(bytes.data() + offset + 4));
        const auto data = offset + 8;
        if (data + size > bytes.size())
            return false;
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && size >= 16) {
            format = read16(bytes.data() + data);
            channels = read16(bytes.data() + data + 2);
            sampleRate = static_cast<int>(read32(bytes.data() + data + 4));
            bits = read16(bytes.data() + data + 14);
            if (format == 0xfffe && size >= 40)
                format = read16(bytes.data() + data + 24);
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            audio = bytes.data() + data;
            audioBytes = size;
        }
        offset = data + size + (size & 1u);
    }
    if (format != 3 || bits != 32 || channels == 0 || sampleRate <= 0 || !audio)
        return false;

    const auto frames = audioBytes / (sizeof(float) * channels);
    mono.assign(frames, 0.0f);
    for (size_t frame = 0; frame < frames; ++frame)
        for (uint16_t channel = 0; channel < channels; ++channel) {
            float value = 0.0f;
            std::memcpy(&value, audio + (frame * channels + channel) * sizeof(float), sizeof(float));
            mono[frame] += value / channels;
        }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: uapmd-mir-tempo-probe FILE.wav [libsonare|librosa]\n";
        return 2;
    }
    const std::string backend = argc == 3 ? argv[2] : "libsonare";
    if (backend != "libsonare" && backend != "librosa") {
        std::cerr << "backend must be 'libsonare' or 'librosa'\n";
        return 2;
    }
    std::vector<float> mono;
    int sampleRate = 0;
    if (!readFloatWave(argv[1], mono, sampleRate)) {
        std::cerr << "only little-endian 32-bit float WAV input is supported\n";
        return 2;
    }

    const auto useLibrosa = backend == "librosa";
    const auto map = useLibrosa
        ? uapmd_mir::librosa_cpp::detectTempoMap(mono, sampleRate, 120.0)
        : uapmd_mir::sonare::detectTempoMap(mono, sampleRate, 120.0);
    std::cout << "backend\t" << backend << '\n';
    std::cout << "tempo\n";
    for (const auto& [time, bpm] : map)
        std::cout << time << '\t' << bpm << '\t'
                  << uapmd_mir::tempoMapTimeToTicks(map, time, 480, map.front().second)
                  << '\n';
    const auto duration = static_cast<double>(mono.size()) / sampleRate;
    std::cout << "end\t" << duration << '\t'
              << uapmd_mir::tempoMapTimeToTicks(map, duration, 480, map.front().second)
              << '\n';
    std::cout << "meter\n";
    const auto meter = useLibrosa
        ? uapmd_mir::librosa_cpp::detectRhythmMap(mono, sampleRate, map, map.front().second)
        : uapmd_mir::sonare::detectRhythmMap(mono, sampleRate, map, map.front().second);
    for (const auto& [time, numerator, denominator] : meter)
        std::cout << time << '\t' << static_cast<int>(numerator) << '/'
                  << static_cast<int>(denominator) << '\t'
                  << uapmd_mir::tempoMapTimeToTicks(map, time, 480, map.front().second)
                  << '\n';
}
