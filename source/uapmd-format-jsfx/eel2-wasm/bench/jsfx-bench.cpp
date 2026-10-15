// Times ysfx_process_float on a JSFX instrument holding a chord, 128-frame blocks at 48kHz.
#include "ysfx.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void sendNote(ysfx_t* fx, uint8_t status, uint8_t note, uint8_t vel) {
    uint8_t bytes[3] = {status, note, vel};
    ysfx_midi_event_t ev{};
    ev.bus = 0;
    ev.offset = 0;
    ev.size = 3;
    ev.data = bytes;
    ysfx_send_midi(fx, &ev);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench <file.jsfx> [voices] [blocks]\n");
        return 1;
    }
    const int voices = argc > 2 ? std::atoi(argv[2]) : 4;
    const int blocks = argc > 3 ? std::atoi(argv[3]) : 1500;
    constexpr uint32_t kFrames = 128;
    constexpr double kRate = 48000.0;

    ysfx_config_t* config = ysfx_config_new();
    ysfx_register_builtin_audio_formats(config);
    ysfx_guess_file_roots(config, argv[1]);
    ysfx_t* fx = ysfx_new(config);
    if (!ysfx_load_file(fx, argv[1], 0) || !ysfx_compile(fx, 0)) {
        std::fprintf(stderr, "load/compile failed: %s\n", argv[1]);
        return 2;
    }
    ysfx_set_sample_rate(fx, kRate);
    ysfx_set_block_size(fx, kFrames);
    ysfx_init(fx);

    std::vector<float> l(kFrames), r(kFrames), il(kFrames), ir(kFrames);
    float* outs[2] = {l.data(), r.data()};
    const float* ins[2] = {il.data(), ir.data()};

    // Warm up, then strike the chord and keep it held for the whole measurement.
    for (int i = 0; i < 20; i++)
        ysfx_process_float(fx, ins, outs, 2, 2, kFrames);
    for (int v = 0; v < voices; v++)
        sendNote(fx, 0x90, static_cast<uint8_t>(48 + v * 4), 100);

    double worst = 0, total = 0, peak = 0;
    for (int b = 0; b < blocks; b++) {
        auto t0 = std::chrono::steady_clock::now();
        ysfx_process_float(fx, ins, outs, 2, 2, kFrames);
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        total += us;
        if (us > worst) worst = us;
        for (uint32_t i = 0; i < kFrames; i++)
            if (std::abs(l[i]) > peak) peak = std::abs(l[i]);
    }
    const double budget = kFrames / kRate * 1e6;
    const double avg = total / blocks;
    std::printf("%-28s voices=%d avg=%8.1fus worst=%8.1fus budget=%.0fus load=%5.1f%% peak=%.3f\n",
                ysfx_get_name(fx), voices, avg, worst, budget, avg / budget * 100.0, peak);
    ysfx_free(fx);
    ysfx_config_free(config);
    return 0;
}
