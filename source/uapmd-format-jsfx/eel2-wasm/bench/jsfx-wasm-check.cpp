// Runs a JSFX instrument interpreted or translated, timing it and writing every output sample for comparison.
#include "ysfx.h"
#include <emscripten.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
    void note(ysfx_t* fx, uint8_t status, uint8_t key, uint8_t velocity) {
        uint8_t bytes[3] = {status, key, velocity};
        ysfx_midi_event_t ev{};
        ev.size = 3;
        ev.data = bytes;
        ysfx_send_midi(fx, &ev);
    }
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: jsfx-wasm-check <file.jsfx> <voices> <blocks> interp|wasm <out.raw>\n");
        return 1;
    }
    const int voices = std::atoi(argv[2]);
    const int blocks = std::atoi(argv[3]);
    const bool wantWasm = !std::strcmp(argv[4], "wasm");
    constexpr uint32_t kFrames = 128;

    // churn: load, translate, run and free the effect repeatedly; the function table must stay bounded
    if (!std::strcmp(argv[4], "churn")) {
        for (int round = 0; round < blocks; round++) {
            ysfx_config_t* c = ysfx_config_new();
            ysfx_guess_file_roots(c, argv[1]);
            ysfx_t* f = ysfx_new(c);
            if (!ysfx_load_file(f, argv[1], 0) || !ysfx_compile(f, 0))
                return 2;
            ysfx_set_sample_rate(f, 48000.0);
            ysfx_set_block_size(f, kFrames);
            ysfx_init(f);
            const bool ok = ysfx_wasm_generate(f) && ysfx_wasm_instantiate(f);
            std::vector<float> l(kFrames), r(kFrames), il(kFrames), ir(kFrames);
            float* o[2] = {l.data(), r.data()};
            const float* in[2] = {il.data(), ir.data()};
            ysfx_process_float(f, in, o, 2, 2, kFrames);
            ysfx_free(f);
            ysfx_config_free(c);
            const int tableLength = EM_ASM_INT({ return wasmTable.length; });
            if (round < 3 || round == blocks - 1)
                std::printf("churn round %d: translated=%d table length=%d\n", round, ok, tableLength);
        }
        return 0;
    }

    ysfx_config_t* config = ysfx_config_new();
    ysfx_register_builtin_audio_formats(config);
    ysfx_guess_file_roots(config, argv[1]);
    ysfx_t* fx = ysfx_new(config);
    if (!ysfx_load_file(fx, argv[1], 0) || !ysfx_compile(fx, 0)) {
        std::fprintf(stderr, "load/compile failed: %s\n", argv[1]);
        return 2;
    }
    ysfx_set_sample_rate(fx, 48000.0);
    ysfx_set_block_size(fx, kFrames);
    ysfx_init(fx);
    bool generated = false, instantiated = false;
    if (wantWasm) {
        generated = ysfx_wasm_generate(fx);
        instantiated = ysfx_wasm_instantiate(fx);
    }

    FILE* out = std::fopen(argv[5], "wb");
    if (!out)
        return 2;
    std::vector<float> l(kFrames), r(kFrames), il(kFrames), ir(kFrames);
    float* outs[2] = {l.data(), r.data()};
    const float* ins[2] = {il.data(), ir.data()};
    double totalUs = 0, worstUs = 0, worstSteadyUs = 0;
    std::vector<double> firstUs;
    for (int b = 0; b < blocks + 20; b++) {
        // a chord after a short warm-up, released halfway through
        if (b == 20)
            for (int v = 0; v < voices; v++) note(fx, 0x90, uint8_t(48 + v * 4), 100);
        if (b == 20 + blocks / 2)
            for (int v = 0; v < voices; v++) note(fx, 0x80, uint8_t(48 + v * 4), 0);
        // a deterministic input, so effects are exercised as well as instruments
        for (uint32_t i = 0; i < kFrames; i++) {
            const double t = double(b * kFrames + i) / 48000.0;
            il[i] = float(0.3 * std::sin(2 * M_PI * 220.0 * t) + 0.1 * std::sin(2 * M_PI * 3517.0 * t));
            ir[i] = -il[i];
        }
        auto t0 = std::chrono::steady_clock::now();
        ysfx_process_float(fx, ins, outs, 2, 2, kFrames);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        totalUs += us;
        if (us > worstUs) worstUs = us;
        if (b < 4) firstUs.push_back(us);
        if (b >= 40 && us > worstSteadyUs) worstSteadyUs = us;
        std::fwrite(l.data(), sizeof(float), kFrames, out);
        std::fwrite(r.data(), sizeof(float), kFrames, out);
    }
    std::fclose(out);
    std::printf("%-30.30s %-6s gen=%d inst=%d avg=%7.1fus worst=%7.1fus steady-worst=%7.1fus first=%.0f/%.0f/%.0f/%.0fus\n",
                ysfx_get_name(fx), argv[4], generated, instantiated, totalUs / (blocks + 20), worstUs, worstSteadyUs,
                firstUs[0], firstUs[1], firstUs[2], firstUs[3]);
    ysfx_free(fx);
    ysfx_config_free(config);
    return 0;
}
