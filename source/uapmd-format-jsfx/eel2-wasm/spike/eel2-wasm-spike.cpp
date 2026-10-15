// Hand-emitted naive EEL2-backend wasm for spike-loop.jsfx's @sample, run on a pthread against the shared heap.
#include <emscripten.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// ---- EEL2-like VM state, all in the shared heap -------------------------------------
constexpr uint32_t kItemsPerBlock = 65536;
constexpr uint32_t kBlocks = 128;
constexpr uint32_t kMaxItems = kItemsPerBlock * kBlocks;

enum Var { I, OUT, PH, S, LP, MEM, SPL0, SPL1, NUM_VARS };
alignas(8) static double g_vars[NUM_VARS];
alignas(8) static double g_ramFail;
static double* g_blocks[kBlocks];

extern "C" EMSCRIPTEN_KEEPALIVE double* spikeRamAlloc(uint32_t block) {
    if (!g_blocks[block])
        g_blocks[block] = static_cast<double*>(std::calloc(kItemsPerBlock, sizeof(double)));
    return g_blocks[block];
}

static double* ramSlot(double index) {
    auto idx = static_cast<uint32_t>(static_cast<int32_t>(index + 0.00001));
    if (idx >= kMaxItems)
        return &g_ramFail;
    return spikeRamAlloc(idx >> 16) + (idx & 0xFFFF);
}

static double callSin(double x) { return std::sin(x); }

// ---- minimal wasm encoder -----------------------------------------------------------
struct Bytes : std::vector<uint8_t> {
    void u8(uint8_t b) { push_back(b); }
    void uleb(uint64_t v) { do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; u8(b); } while (v); }
    void sleb(int64_t v) {
        bool more = true;
        while (more) {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) more = false; else b |= 0x80;
            u8(b);
        }
    }
    void str(const char* s) { auto n = std::strlen(s); uleb(n); insert(end(), s, s + n); }
    void f64(double d) { uint8_t b[8]; std::memcpy(b, &d, 8); insert(end(), b, b + 8); }
    void append(const Bytes& o) { insert(end(), o.begin(), o.end()); }
    void section(uint8_t id, const Bytes& body) { u8(id); uleb(body.size()); append(body); }
};

namespace op {
    constexpr uint8_t Block = 0x02, Loop = 0x03, If = 0x04, End = 0x0B, Br = 0x0C, BrIf = 0x0D, Return = 0x0F,
                      Call = 0x10, LocalGet = 0x20, LocalSet = 0x21, LocalTee = 0x22, I32Load = 0x28,
                      F64Load = 0x2B, F64Store = 0x39, I32Const = 0x41, F64Const = 0x44, I32Eqz = 0x45,
                      I32GeU = 0x4F, F64Ge = 0x66, I32Add = 0x6A, I32Sub = 0x6B, I32And = 0x71, I32Shl = 0x74,
                      I32ShrU = 0x76, F64Add = 0xA0, F64Sub = 0xA1, F64Mul = 0xA2, Void = 0x40,
                      TyI32 = 0x7F, TyF64 = 0x7C;
}

struct Emitter {
    Bytes c;
    uint32_t memaddrFunc;
    uint32_t sinFunc;

    void i32c(int32_t v) { c.u8(op::I32Const); c.sleb(v); }
    void f64c(double v) { c.u8(op::F64Const); c.f64(v); }
    void f64load(uint32_t offset) { c.u8(op::F64Load); c.uleb(3); c.uleb(offset); }
    void f64store(uint32_t offset) { c.u8(op::F64Store); c.uleb(3); c.uleb(offset); }
    uint32_t varAddr(Var v) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_vars[v])); }
    void loadVar(Var v) { i32c(0); f64load(varAddr(v)); }
    template <typename F> void storeVar(Var v, F value) { i32c(0); value(); f64store(varAddr(v)); }
    // mem-style access: leaves the slot's address on the stack.
    template <typename F> void ramAddr(F index) { index(); c.u8(op::Call); c.uleb(memaddrFunc); }
};

// (f64 index) -> i32 address, the inlined fast path of EEL2's __NSEEL_RAMAlloc.
static Bytes emitMemaddrBody(uint32_t ramAllocFunc) {
    Emitter e{};
    auto& c = e.c;
    const auto blocksAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_blocks));
    const auto failAddr = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&g_ramFail));
    c.uleb(1); c.uleb(2); c.u8(op::TyI32); // locals: idx, blk
    c.u8(op::LocalGet); c.uleb(0); e.f64c(0.00001); c.u8(op::F64Add);
    c.u8(0xFC); c.uleb(2); // i32.trunc_sat_f64_s
    c.u8(op::LocalSet); c.uleb(1);
    c.u8(op::LocalGet); c.uleb(1); e.i32c(static_cast<int32_t>(kMaxItems)); c.u8(op::I32GeU);
    c.u8(op::If); c.u8(op::Void); e.i32c(failAddr); c.u8(op::Return); c.u8(op::End);
    c.u8(op::LocalGet); c.uleb(1); e.i32c(16); c.u8(op::I32ShrU); e.i32c(2); c.u8(op::I32Shl);
    c.u8(op::I32Load); c.uleb(2); c.uleb(blocksAddr);
    c.u8(op::LocalTee); c.uleb(2); c.u8(op::I32Eqz);
    c.u8(op::If); c.u8(op::Void);
    c.u8(op::LocalGet); c.uleb(1); e.i32c(16); c.u8(op::I32ShrU);
    c.u8(op::Call); c.uleb(ramAllocFunc); c.u8(op::LocalSet); c.uleb(2);
    c.u8(op::End);
    c.u8(op::LocalGet); c.uleb(2);
    c.u8(op::LocalGet); c.uleb(1); e.i32c(0xFFFF); c.u8(op::I32And); e.i32c(3); c.u8(op::I32Shl);
    c.u8(op::I32Add);
    c.u8(op::End);
    return c;
}

static Bytes emitSampleBody(uint32_t memaddrFunc, uint32_t sinFunc) {
    Emitter e{};
    e.memaddrFunc = memaddrFunc;
    e.sinFunc = sinFunc;
    auto& c = e.c;
    using namespace op;
    c.uleb(1); c.uleb(1); c.u8(TyI32); // local: loop counter
    // mem + i*4 + k
    auto index = [&](double k) {
        return [&e, k] {
            e.loadVar(MEM); e.loadVar(I); e.f64c(4); e.c.u8(op::F64Mul); e.c.u8(op::F64Add);
            if (k != 0) { e.f64c(k); e.c.u8(op::F64Add); }
        };
    };
    e.storeVar(I, [&] { e.f64c(0); });
    e.storeVar(OUT, [&] { e.f64c(0); });
    e.i32c(16); c.u8(LocalSet); c.uleb(0);
    c.u8(Block); c.u8(Void); c.u8(Loop); c.u8(Void);
    // ph = mem[i*4]
    e.storeVar(PH, [&] { e.ramAddr(index(0)); e.f64load(0); });
    // ph += mem[i*4+1]
    e.storeVar(PH, [&] { e.loadVar(PH); e.ramAddr(index(1)); e.f64load(0); c.u8(F64Add); });
    // ph >= 1 ? ph -= 1
    e.loadVar(PH); e.f64c(1); c.u8(F64Ge);
    c.u8(If); c.u8(Void); e.storeVar(PH, [&] { e.loadVar(PH); e.f64c(1); c.u8(F64Sub); }); c.u8(End);
    // mem[i*4] = ph
    e.ramAddr(index(0)); e.loadVar(PH); e.f64store(0);
    // s = sin(ph * 2 * $pi)
    e.storeVar(S, [&] { e.loadVar(PH); e.f64c(2); c.u8(F64Mul); e.f64c(M_PI); c.u8(F64Mul); c.u8(Call); c.uleb(sinFunc); });
    // lp = mem[i*4+2]
    e.storeVar(LP, [&] { e.ramAddr(index(2)); e.f64load(0); });
    // lp += (s - lp) * 0.1
    e.storeVar(LP, [&] { e.loadVar(LP); e.loadVar(S); e.loadVar(LP); c.u8(F64Sub); e.f64c(0.1); c.u8(F64Mul); c.u8(F64Add); });
    // mem[i*4+2] = lp
    e.ramAddr(index(2)); e.loadVar(LP); e.f64store(0);
    // out += lp
    e.storeVar(OUT, [&] { e.loadVar(OUT); e.loadVar(LP); c.u8(F64Add); });
    // i += 1
    e.storeVar(I, [&] { e.loadVar(I); e.f64c(1); c.u8(F64Add); });
    c.u8(LocalGet); c.uleb(0); e.i32c(1); c.u8(I32Sub); c.u8(LocalTee); c.uleb(0); c.u8(BrIf); c.uleb(0);
    c.u8(End); c.u8(End);
    e.storeVar(SPL0, [&] { e.loadVar(OUT); e.f64c(0.05); c.u8(F64Mul); });
    e.storeVar(SPL1, [&] { e.loadVar(SPL0); });
    c.u8(End);
    return c;
}

static Bytes buildModule() {
    using namespace op;
    Bytes m;
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    m.insert(m.end(), header, header + 8);

    Bytes types; // 0:(f64)->f64 1:(i32)->i32 2:()->() 3:(f64)->i32
    types.uleb(4);
    types.u8(0x60); types.uleb(1); types.u8(TyF64); types.uleb(1); types.u8(TyF64);
    types.u8(0x60); types.uleb(1); types.u8(TyI32); types.uleb(1); types.u8(TyI32);
    types.u8(0x60); types.uleb(0); types.uleb(0);
    types.u8(0x60); types.uleb(1); types.u8(TyF64); types.uleb(1); types.u8(TyI32);
    m.section(1, types);

    Bytes imports; // funcs 0:sin 1:ramAlloc, then the shared heap
    imports.uleb(3);
    imports.str("env"); imports.str("sin"); imports.u8(0x00); imports.uleb(0);
    imports.str("env"); imports.str("ramAlloc"); imports.u8(0x00); imports.uleb(1);
    imports.str("env"); imports.str("memory"); imports.u8(0x02); imports.u8(0x03); imports.uleb(1); imports.uleb(65536);
    m.section(2, imports);

    Bytes funcs; // 2:memaddr 3:sample
    funcs.uleb(2); funcs.uleb(3); funcs.uleb(2);
    m.section(3, funcs);

    Bytes exports;
    exports.uleb(1); exports.str("sample"); exports.u8(0x00); exports.uleb(3);
    m.section(7, exports);

    Bytes code;
    code.uleb(2);
    auto memaddr = emitMemaddrBody(1);
    code.uleb(memaddr.size()); code.append(memaddr);
    auto sample = emitSampleBody(2, 0);
    code.uleb(sample.size()); code.append(sample);
    m.section(10, code);
    return m;
}

// ---- runtime plumbing ---------------------------------------------------------------
EM_JS_DEPS(spike_deps, "$addFunction");

// Instantiates on the calling thread and returns `sample`'s index in this thread's table (0 on failure).
EM_JS(int, spike_instantiate, (const uint8_t* bytes, int len, int sinPtr, int allocPtr), {
    try {
        var mod = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + len));
        var inst = new WebAssembly.Instance(mod, { env: {
            memory: wasmMemory, sin: wasmTable.get(sinPtr), ramAlloc: wasmTable.get(allocPtr) } });
        return addFunction(inst.exports.sample, 'v');
    } catch (e) {
        err('spike_instantiate: ' + e);
        return 0;
    }
});

// What this thread's table holds at `index`: -1 out of range, 0 null, 1 a function.
EM_JS(int, spike_table_probe, (int index), {
    if (index >= wasmTable.length) return -1;
    return wasmTable.get(index) ? 1 : 0;
});

EM_JS(int, spike_table_length, (), { return wasmTable.length; });

// ---- reference & benchmark ----------------------------------------------------------
static void resetState() {
    std::memset(g_vars, 0, sizeof(g_vars));
    for (auto& b : g_blocks) { std::free(b); b = nullptr; }
    for (int i = 0; i < 16; i++)
        *ramSlot(g_vars[MEM] + i * 4 + 1) = 110.0 * (i + 1) / 48000.0;
}

// The same @sample in plain C++, through the same RAM paging: what ideal codegen looks like.
__attribute__((noinline)) static void referenceSample() {
    g_vars[I] = 0;
    g_vars[OUT] = 0;
    for (int n = 0; n < 16; n++) {
        g_vars[PH] = *ramSlot(g_vars[MEM] + g_vars[I] * 4);
        g_vars[PH] = g_vars[PH] + *ramSlot(g_vars[MEM] + g_vars[I] * 4 + 1);
        if (g_vars[PH] >= 1) g_vars[PH] = g_vars[PH] - 1;
        *ramSlot(g_vars[MEM] + g_vars[I] * 4) = g_vars[PH];
        g_vars[S] = callSin(g_vars[PH] * 2 * M_PI);
        g_vars[LP] = *ramSlot(g_vars[MEM] + g_vars[I] * 4 + 2);
        g_vars[LP] = g_vars[LP] + (g_vars[S] - g_vars[LP]) * 0.1;
        *ramSlot(g_vars[MEM] + g_vars[I] * 4 + 2) = g_vars[LP];
        g_vars[OUT] = g_vars[OUT] + g_vars[LP];
        g_vars[I] = g_vars[I] + 1;
    }
    g_vars[SPL0] = g_vars[OUT] * 0.05;
    g_vars[SPL1] = g_vars[SPL0];
}

struct Result { double avg, worst, peak; std::vector<double> trace; double first[5]; };

static Result run(void (*sample)(), int blocks) {
    resetState();
    Result r{0, 0, 0, {}};
    for (int b = 0; b < blocks; b++) {
        auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < 128; f++) {
            sample();
            if (b < 8) r.trace.push_back(g_vars[SPL0]);
            if (std::fabs(g_vars[SPL0]) > r.peak) r.peak = std::fabs(g_vars[SPL0]);
        }
        auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (b < 5) r.first[b] = us;
        r.avg += us;
        if (us > r.worst) r.worst = us;
    }
    r.avg /= blocks;
    return r;
}

int main() {
    const int kBlocksToRun = 1500;
    const auto module = buildModule();
    std::printf("module: %zu bytes\n", module.size());
    const int sinPtr = static_cast<int>(reinterpret_cast<intptr_t>(&callSin));
    const int allocPtr = static_cast<int>(reinterpret_cast<intptr_t>(&spikeRamAlloc));

    std::atomic<int> indexA{0};
    std::atomic<bool> bDone{false};
    std::atomic<bool> aMeasured{false};
    std::thread a([&] {
        auto t0 = std::chrono::steady_clock::now();
        const int index = spike_instantiate(module.data(), static_cast<int>(module.size()), sinPtr, allocPtr);
        auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[thread A] compile+instantiate+publish: %.1fus -> table index %d (table length %d)\n",
                    us, index, spike_table_length());
        indexA = index;
        if (!index) {
            aMeasured = true;
            return;
        }
        auto generated = reinterpret_cast<void (*)()>(static_cast<intptr_t>(index));
        auto ref = run(&referenceSample, kBlocksToRun);
        auto gen = run(generated, kBlocksToRun);
        double maxDiff = 0;
        for (size_t i = 0; i < ref.trace.size(); i++)
            maxDiff = std::fmax(maxDiff, std::fabs(ref.trace[i] - gen.trace[i]));
        std::printf("[thread A] C++ reference : avg=%7.1fus worst=%7.1fus peak=%.3f\n", ref.avg, ref.worst, ref.peak);
        std::printf("[thread A] generated wasm: avg=%7.1fus worst=%7.1fus peak=%.3f  max|diff| vs ref over %zu samples = %g\n",
                    gen.avg, gen.worst, gen.peak, ref.trace.size(), maxDiff);
        std::printf("[thread A] generated wasm first blocks: %.1f %.1f %.1f %.1f %.1f us\n",
                    gen.first[0], gen.first[1], gen.first[2], gen.first[3], gen.first[4]);
        aMeasured = true;
        while (!bDone.load()) std::this_thread::yield();
    });
    while (!indexA.load()) std::this_thread::yield();

    std::thread b([&] {
        const int index = indexA.load();
        std::printf("[thread B] before instantiating: table length %d, probe(index %d) = %d\n",
                    spike_table_length(), index, spike_table_probe(index));
        const int own = spike_instantiate(module.data(), static_cast<int>(module.size()), sinPtr, allocPtr);
        std::printf("[thread B] after instantiating: own index %d, probe(%d) = %d\n", own, index, spike_table_probe(index));
        while (!aMeasured.load()) std::this_thread::yield();
        if (own) {
            auto gen = run(reinterpret_cast<void (*)()>(static_cast<intptr_t>(own)), 200);
            std::printf("[thread B] generated wasm: avg=%7.1fus peak=%.3f\n", gen.avg, gen.peak);
        }
        bDone = true;
    });
    b.join();
    a.join();
    std::printf("[main] table length %d, probe(index %d) = %d\n", spike_table_length(), indexA.load(),
                spike_table_probe(indexA.load()));
    return 0;
}
