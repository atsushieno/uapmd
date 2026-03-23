// uapmd-wclap-host.wasm — bridge between wclap.mjs and CLAP plugins.
//
// Five exported functions are called by wclap.mjs to manage cross-WASM
// plugin instances (_wclapInstanceCreate, _wclapInstanceSetPath, etc.).
//
// Seven additional exported functions drive the CLAP plugin lifecycle and
// per-quantum audio rendering (_wclapPluginSetup, _wclapPluginProcess, etc.).
// They are called from uapmd-webclap-worklet.js on the AudioWorklet thread.
//
// This module is compiled as wasm32 standalone (no Emscripten JS glue).
// wclap32:: types (Pointer<>, Function<>) therefore match the plugin WASM
// layout exactly, so host structs can be memcpy'd into plugin memory via
// Instance::setArray().

#include <cstddef>  // size_t — must precede wclap-host-cpp headers
#include "wclap-js-instance.h"

#include <atomic>
#include <array>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Bring only what we need from wclap32 into scope without pulling in the
// wclap:: template class, which would make the global `using Instance = ...`
// typedef ambiguous.
using wclap32::Pointer;
using wclap32::wclap_audio_buffer;
using wclap32::wclap_event_header;
using wclap32::wclap_event_midi2;
using wclap32::wclap_event_param_value;
using wclap32::wclap_host;
using wclap32::wclap_input_events;
using wclap32::wclap_output_events;
using wclap32::wclap_param_info;
using wclap32::wclap_plugin;
using wclap32::wclap_plugin_entry;
using wclap32::wclap_plugin_factory;
using wclap32::wclap_plugin_params;
using wclap32::wclap_process;

// ── Instance management (called by wclap.mjs) ─────────────────────────────

Instance * _wclapInstanceCreate(bool is64) {
    return new Instance(is64);
}

char * _wclapInstanceSetPath(Instance *instance, size_t size) {
    auto *impl = (js_wasm::WclapInstance *)instance;
    impl->pathChars.assign(size + 1, '\0');
    return impl->pathChars.data();
}

static std::atomic<uint32_t> indexCounter{0};
uint32_t _wclapInstanceGetNextIndex() { return indexCounter++; }

static std::atomic<uint32_t> hostThreadCounter{0};
int32_t _wclapNextThreadId() {
    auto id = ++hostThreadCounter;
    if (id >= 0x20000000u) { --hostThreadCounter; return -1; }
    return static_cast<int32_t>(id);
}

int32_t _wclapStartInstanceThread(Instance *, uint64_t) { return -1; }

// ── Null host callbacks registered before plugin init ─────────────────────

struct SlotState;

static Pointer<const void>
host_get_ext(void *, Pointer<const wclap_host>, Pointer<const char>) {
    return {0};
}
static void host_req_restart (void *, Pointer<const wclap_host>) {}
static void host_req_process (void *, Pointer<const wclap_host>) {}
static void host_req_callback(void *, Pointer<const wclap_host>) {}

static uint32_t in_events_size(void *ctx, Pointer<const wclap_input_events>);
static Pointer<const wclap_event_header> in_events_get(void *ctx, Pointer<const wclap_input_events>, uint32_t index);

static bool
out_events_try_push(void *, Pointer<const wclap_output_events>,
                    Pointer<const wclap_event_header>) { return true; }

extern "C" __attribute__((export_name("_wclapHostGetExt")))
Pointer<const void> _wclapHostGetExt(void *ctx, Pointer<const wclap_host> host, Pointer<const char> id) {
    return host_get_ext(ctx, host, id);
}

extern "C" __attribute__((export_name("_wclapHostReqRestart")))
void _wclapHostReqRestart(void *ctx, Pointer<const wclap_host> host) {
    host_req_restart(ctx, host);
}

extern "C" __attribute__((export_name("_wclapHostReqProcess")))
void _wclapHostReqProcess(void *ctx, Pointer<const wclap_host> host) {
    host_req_process(ctx, host);
}

extern "C" __attribute__((export_name("_wclapHostReqCallback")))
void _wclapHostReqCallback(void *ctx, Pointer<const wclap_host> host) {
    host_req_callback(ctx, host);
}

// ── Per-slot state ─────────────────────────────────────────────────────────
// One SlotState is created by _wclapPluginSetup and destroyed by
// _wclapPluginDestroy. All uint32_t members are byte-offsets into the
// *plugin* WASM linear memory; localOut* are in *host* WASM heap memory.

struct SlotState {
    static constexpr uint32_t kMaxInputEvents = 64;
    static constexpr uint32_t kInputEventStride = 64;

    uint32_t plugin_ptr    = 0;
    uint32_t in_data_ptr   = 0; // float[2][maxFrames] — ch0 then ch1
    uint32_t in_ptrs_ptr   = 0; // float*[2]
    uint32_t in_buf_ptr    = 0; // wclap_audio_buffer (stereo input bus)
    uint32_t out_data_ptr  = 0; // float[2][maxFrames] — ch0 then ch1
    uint32_t out_ptrs_ptr  = 0; // float*[2]
    uint32_t out_buf_ptr   = 0; // wclap_audio_buffer (stereo output bus)
    uint32_t in_events_ptr = 0; // wclap_input_events
    uint32_t input_events_data_ptr = 0; // kMaxInputEvents * kInputEventStride
    uint32_t out_events_ptr= 0; // wclap_output_events (discard)
    uint32_t process_ptr   = 0; // wclap_process
    uint32_t maxFrames     = 0;
    uint32_t input_event_stride = kInputEventStride;
    uint32_t queued_input_events = 0;
    int64_t  steady_time   = 0;
    bool has_params = false;
    wclap_plugin_params params{};
    std::vector<wclap_param_info> parameter_infos{};
    std::string parameter_json{};
    float localInL[2048]{};
    float localInR[2048]{};
    float localOutL[2048]{};
    float localOutR[2048]{};
};

static uint32_t in_events_size(void *ctx, Pointer<const wclap_input_events>) {
    auto *state = static_cast<SlotState *>(ctx);
    return state ? state->queued_input_events : 0;
}

static Pointer<const wclap_event_header>
in_events_get(void *ctx, Pointer<const wclap_input_events>, uint32_t index) {
    auto *state = static_cast<SlotState *>(ctx);
    if (!state || index >= state->queued_input_events)
        return {0};
    return Pointer<const wclap_event_header>{
        state->input_events_data_ptr + index * state->input_event_stride};
}

extern "C" __attribute__((export_name("_wclapInEventsSize")))
uint32_t _wclapInEventsSize(void *ctx, Pointer<const wclap_input_events> events) {
    return in_events_size(ctx, events);
}

extern "C" __attribute__((export_name("_wclapInEventsGet")))
Pointer<const wclap_event_header> _wclapInEventsGet(
    void *ctx, Pointer<const wclap_input_events> events, uint32_t index)
{
    return in_events_get(ctx, events, index);
}

extern "C" __attribute__((export_name("_wclapOutEventsTryPush")))
bool _wclapOutEventsTryPush(
    void *ctx, Pointer<const wclap_output_events> events, Pointer<const wclap_event_header> header)
{
    return out_events_try_push(ctx, events, header);
}

static std::unordered_map<Instance *, SlotState *> s_slots;

// Copy count elements of T from host memory into plugin memory at destOffset.
template<class T>
static inline void writePlugin(Instance *inst, uint32_t destOffset,
                                const T *src, uint32_t count) {
    inst->setArray(Pointer<T>{destOffset}, const_cast<T *>(src), count);
}

// Copy count elements of T from plugin memory at srcOffset into host memory.
template<class T>
static inline void readPlugin(Instance *inst, uint32_t srcOffset,
                               T *dst, uint32_t count) {
    inst->getArray(Pointer<T>{srcOffset}, dst, count);
}

// Allocate size bytes in plugin memory, write data, return the address.
static uint32_t allocInPlugin(Instance *inst, const void *data, uint32_t size) {
    auto p = inst->malloc32(size);
    if (p.wasmPointer == 0) return 0;
    writePlugin(inst, p.wasmPointer,
                reinterpret_cast<const uint8_t *>(data), size);
    return p.wasmPointer;
}

static std::string jsonEscape(const char *text, size_t size) {
    std::string escaped;
    escaped.reserve(size + 8);
    for (size_t i = 0; i < size && text[i] != '\0'; ++i) {
        switch (text[i]) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += text[i]; break;
        }
    }
    return escaped;
}

static void rebuildParameterJson(Instance *inst, SlotState *state, Pointer<const wclap_plugin> plugPtr) {
    if (!state || !state->has_params)
        return;

    state->parameter_infos.clear();
    state->parameter_json = "[]";

    auto count = inst->call(state->params.count, plugPtr);
    if (count == 0)
        return;

    auto infoPtr = inst->malloc32(sizeof(wclap_param_info));
    if (!infoPtr.wasmPointer)
        return;

    state->parameter_infos.reserve(count);
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (uint32_t index = 0; index < count; ++index) {
        if (!inst->call(state->params.get_info, plugPtr, index, Pointer<wclap_param_info>{infoPtr.wasmPointer}))
            continue;

        wclap_param_info info{};
        readPlugin(inst, infoPtr.wasmPointer, &info, 1);
        double currentValue = info.default_value;
        double tmpValue = 0.0;
        if (inst->call(state->params.get_value, plugPtr, info.id, Pointer<double>{infoPtr.wasmPointer}))
            readPlugin(inst, infoPtr.wasmPointer, &tmpValue, 1);
        else
            tmpValue = info.default_value;
        currentValue = tmpValue;

        state->parameter_infos.emplace_back(info);

        if (!first)
            json << ",";
        first = false;
        json << "{"
             << "\"index\":" << index
             << ",\"id\":\"" << info.id << "\""
             << ",\"name\":\"" << jsonEscape(info.name, sizeof(info.name)) << "\""
             << ",\"path\":\"" << jsonEscape(info.module, sizeof(info.module)) << "\""
             << ",\"minValue\":" << info.min_value
             << ",\"maxValue\":" << info.max_value
             << ",\"defaultValue\":" << info.default_value
             << ",\"currentValue\":" << currentValue
             << ",\"automatable\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_AUTOMATABLE) ? "true" : "false")
             << ",\"hidden\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_HIDDEN) ? "true" : "false")
             << ",\"readOnly\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_READONLY) ? "true" : "false")
             << ",\"stepped\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_STEPPED) ? "true" : "false")
             << "}";
    }
    json << "]";
    state->parameter_json = json.str();
}

static bool enqueueInputEvent(Instance *inst, SlotState *state, const void *eventData, uint32_t eventSize) {
    if (!inst || !state || !eventData || eventSize == 0 || eventSize > state->input_event_stride)
        return false;
    if (state->queued_input_events >= SlotState::kMaxInputEvents)
        return false;
    const uint32_t dest = state->input_events_data_ptr
        + state->queued_input_events * state->input_event_stride;
    std::array<uint8_t, SlotState::kInputEventStride> storage{};
    std::memcpy(storage.data(), eventData, eventSize);
    writePlugin(inst, dest, storage.data(), state->input_event_stride);
    ++state->queued_input_events;
    return true;
}

// ── Plugin lifecycle exports ───────────────────────────────────────────────

// _wclapPluginSetup
//
// Must be called once per slot after host.startWclap() returns (so the
// Instance* exists) and before inst->init() has been called (so
// registerHost32 calls are still valid — hadInit is still false in JS).
//
// Returns 1 on success, 0 on any failure.
extern "C" __attribute__((export_name("_wclapPluginSetup")))
int32_t _wclapPluginSetup(Instance *inst,
                          double sampleRate, int32_t minBuf, int32_t maxBuf)
{
    if (!inst || maxBuf <= 0 || maxBuf > 2048) return 0;
    auto maxF = static_cast<uint32_t>(maxBuf);
    auto *state = new SlotState{};

    // Register host callbacks before init() so the plugin can call back.
    wclap_host h{};
    h.clap_version = {1, 2, 0};
    h.host_data    = {0};
    h.name = h.vendor = h.url = h.version = {0};
    h.get_extension   = inst->registerHost32<Pointer<const void>,
                            Pointer<const wclap_host>, Pointer<const char>>(
                                state, host_get_ext);
    h.request_restart  = inst->registerHost32<void,
                            Pointer<const wclap_host>>(state, host_req_restart);
    h.request_process  = inst->registerHost32<void,
                            Pointer<const wclap_host>>(state, host_req_process);
    h.request_callback = inst->registerHost32<void,
                            Pointer<const wclap_host>>(state, host_req_callback);

    wclap_input_events ie{};
    ie.ctx  = {0};
    ie.size = inst->registerHost32<uint32_t, Pointer<const wclap_input_events>>(
                  state, in_events_size);
    ie.get  = inst->registerHost32<Pointer<const wclap_event_header>,
                  Pointer<const wclap_input_events>, uint32_t>(
                      state, in_events_get);

    wclap_output_events oe{};
    oe.ctx      = {0};
    oe.try_push = inst->registerHost32<bool, Pointer<const wclap_output_events>,
                      Pointer<const wclap_event_header>>(
                          state, out_events_try_push);

    // init() sets hadInit in JS, making subsequent registerHost32 calls throw.
    if (!inst->init()) {
        std::cerr << "[wclap] inst->init() failed\n";
        delete state;
        return 0;
    }

    // entry.init("/")
    static const uint8_t kRoot[] = {'/', '\0'};
    uint32_t pathPtr = allocInPlugin(inst, kRoot, sizeof(kRoot));
    if (!pathPtr) {
        delete state;
        return 0;
    }

    auto entryVal = inst->get(inst->entry32);
    if (entryVal.init.wasmPointer)
        inst->call(entryVal.init, Pointer<const char>{pathPtr});

    // get_factory("clap.plugin-factory")
    static const char kFactoryId[] = "clap.plugin-factory";
    uint32_t fidPtr = allocInPlugin(inst,
                          reinterpret_cast<const uint8_t *>(kFactoryId),
                          sizeof(kFactoryId));
    if (!fidPtr) {
        delete state;
        return 0;
    }

    auto factVoid = inst->call(entryVal.get_factory,
                               Pointer<const char>{fidPtr});
    if (factVoid.wasmPointer == 0) {
        std::cerr << "[wclap] get_factory returned null\n";
        delete state;
        return 0;
    }
    auto factPtr = Pointer<const wclap_plugin_factory>{factVoid.wasmPointer};
    auto factVal = inst->get(factPtr);

    uint32_t pluginCount = inst->call(factVal.get_plugin_count, factPtr);
    if (pluginCount == 0) {
        std::cerr << "[wclap] plugin factory is empty\n";
        delete state;
        return 0;
    }

    // Read the first plugin descriptor's ID string.
    auto descPtr = inst->call(factVal.get_plugin_descriptor, factPtr, 0u);
    if (descPtr.wasmPointer == 0) {
        std::cerr << "[wclap] get_plugin_descriptor(0) returned null\n";
        delete state;
        return 0;
    }
    auto descVal = inst->get(descPtr);
    std::string pluginId = inst->getString(descVal.id, 256);

    uint32_t idPtr = allocInPlugin(inst,
                         reinterpret_cast<const uint8_t *>(pluginId.c_str()),
                         static_cast<uint32_t>(pluginId.size() + 1));
    if (!idPtr) {
        delete state;
        return 0;
    }

    uint32_t hostPtr = allocInPlugin(inst,
                           reinterpret_cast<const uint8_t *>(&h),
                           sizeof(h));
    if (!hostPtr) {
        delete state;
        return 0;
    }

    // create_plugin / init / activate
    auto plugPtr = inst->call(factVal.create_plugin,
                              factPtr,
                              Pointer<const wclap_host>{hostPtr},
                              Pointer<const char>{idPtr});
    if (plugPtr.wasmPointer == 0) {
        std::cerr << "[wclap] create_plugin failed\n";
        delete state;
        return 0;
    }
    auto plugVal = inst->get(plugPtr);

    if (!inst->call(plugVal.init, plugPtr)) {
        std::cerr << "[wclap] plugin.init() failed\n";
        delete state;
        return 0;
    }
    if (!inst->call(plugVal.activate, plugPtr, sampleRate,
                    static_cast<uint32_t>(minBuf),
                    static_cast<uint32_t>(maxBuf))) {
        std::cerr << "[wclap] plugin.activate() failed\n";
        delete state;
        return 0;
    }

    // Pre-allocate audio and process structs in plugin memory so the render
    // loop is allocation-free.
    uint32_t inDataBytes = maxF * 2 * sizeof(float);
    auto inDataP = inst->malloc32(inDataBytes);
    if (!inDataP.wasmPointer) {
        delete state;
        return 0;
    }
    {
        std::vector<float> zeros(maxF * 2, 0.0f);
        writePlugin(inst, inDataP.wasmPointer, zeros.data(), maxF * 2);
    }

    auto inPtrsP = inst->malloc32(2 * sizeof(uint32_t));
    if (!inPtrsP.wasmPointer) {
        delete state;
        return 0;
    }
    {
        uint32_t ptrs[2] = {
            inDataP.wasmPointer,
            inDataP.wasmPointer + maxF * sizeof(float)
        };
        writePlugin(inst, inPtrsP.wasmPointer, ptrs, 2);
    }

    wclap_audio_buffer inBuf{};
    inBuf.data32        = Pointer<Pointer<float>>{inPtrsP.wasmPointer};
    inBuf.data64        = Pointer<Pointer<double>>{0};
    inBuf.channel_count = 2;
    inBuf.latency       = 0;
    inBuf.constant_mask = 0;
    uint32_t inBufPtr = allocInPlugin(inst,
                            reinterpret_cast<const uint8_t *>(&inBuf),
                            sizeof(inBuf));
    if (!inBufPtr) {
        delete state;
        return 0;
    }

    uint32_t outDataBytes = maxF * 2 * sizeof(float);
    auto outDataP = inst->malloc32(outDataBytes);
    if (!outDataP.wasmPointer) {
        delete state;
        return 0;
    }
    {
        std::vector<float> zeros(maxF * 2, 0.0f);
        writePlugin(inst, outDataP.wasmPointer, zeros.data(), maxF * 2);
    }

    auto outPtrsP = inst->malloc32(2 * sizeof(uint32_t));
    if (!outPtrsP.wasmPointer) {
        delete state;
        return 0;
    }
    {
        uint32_t ptrs[2] = {
            outDataP.wasmPointer,
            outDataP.wasmPointer + maxF * sizeof(float)
        };
        writePlugin(inst, outPtrsP.wasmPointer, ptrs, 2);
    }

    wclap_audio_buffer outBuf{};
    outBuf.data32        = Pointer<Pointer<float>>{outPtrsP.wasmPointer};
    outBuf.data64        = Pointer<Pointer<double>>{0};
    outBuf.channel_count = 2;
    outBuf.latency       = 0;
    outBuf.constant_mask = 0;
    uint32_t outBufPtr = allocInPlugin(inst,
                             reinterpret_cast<const uint8_t *>(&outBuf),
                             sizeof(outBuf));
    if (!outBufPtr) {
        delete state;
        return 0;
    }

    auto inputEventsData = inst->malloc32(SlotState::kMaxInputEvents * SlotState::kInputEventStride);
    if (!inputEventsData.wasmPointer) {
        delete state;
        return 0;
    }
    state->input_events_data_ptr = inputEventsData.wasmPointer;

    uint32_t iePtr = allocInPlugin(inst,
                         reinterpret_cast<const uint8_t *>(&ie), sizeof(ie));
    if (!iePtr) {
        delete state;
        return 0;
    }

    uint32_t oePtr = allocInPlugin(inst,
                         reinterpret_cast<const uint8_t *>(&oe), sizeof(oe));
    if (!oePtr) {
        delete state;
        return 0;
    }

    wclap_process proc{};
    proc.steady_time         = 0;
    proc.frames_count        = static_cast<uint32_t>(maxBuf);
    proc.transport           = {0};
    proc.audio_inputs        = Pointer<wclap_audio_buffer>{inBufPtr};
    proc.audio_outputs       = Pointer<wclap_audio_buffer>{outBufPtr};
    proc.audio_inputs_count  = 1;
    proc.audio_outputs_count = 1;
    proc.in_events           = Pointer<const wclap_input_events>{iePtr};
    proc.out_events          = Pointer<const wclap_output_events>{oePtr};
    uint32_t procPtr = allocInPlugin(inst,
                           reinterpret_cast<const uint8_t *>(&proc),
                           sizeof(proc));
    if (!procPtr) {
        delete state;
        return 0;
    }

    state->plugin_ptr     = plugPtr.wasmPointer;
    state->in_data_ptr    = inDataP.wasmPointer;
    state->in_ptrs_ptr    = inPtrsP.wasmPointer;
    state->in_buf_ptr     = inBufPtr;
    state->out_data_ptr   = outDataP.wasmPointer;
    state->out_ptrs_ptr   = outPtrsP.wasmPointer;
    state->out_buf_ptr    = outBufPtr;
    state->in_events_ptr  = iePtr;
    state->out_events_ptr = oePtr;
    state->process_ptr    = procPtr;
    state->maxFrames      = maxF;
    state->steady_time    = 0;

    static const char kParamsExtId[] = "clap.params";
    uint32_t paramsIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t *>(kParamsExtId),
        sizeof(kParamsExtId));
    if (paramsIdPtr) {
        auto paramsVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{paramsIdPtr});
        if (paramsVoid.wasmPointer != 0) {
            state->has_params = true;
            state->params = inst->get(Pointer<const wclap_plugin_params>{paramsVoid.wasmPointer});
            rebuildParameterJson(inst, state, plugPtr);
        }
    }
    s_slots[inst]         = state;

    return 1;
}

extern "C" __attribute__((export_name("_wclapPluginStartProcessing")))
void _wclapPluginStartProcessing(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end()) return;
    auto plugPtr = Pointer<const wclap_plugin>{it->second->plugin_ptr};
    auto plugVal = inst->get(plugPtr);
    inst->call(plugVal.start_processing, plugPtr);
}

extern "C" __attribute__((export_name("_wclapPluginStopProcessing")))
void _wclapPluginStopProcessing(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end()) return;
    auto plugPtr = Pointer<const wclap_plugin>{it->second->plugin_ptr};
    auto plugVal = inst->get(plugPtr);
    inst->call(plugVal.stop_processing, plugPtr);
}

// _wclapPluginProcess
//
// Called from the AudioWorklet process() on every engine quantum.
// Updates steady_time and frames_count in the pre-allocated process struct,
// zeros plugin output buffers, calls plugin.process(), then copies ch0/ch1
// from plugin memory into state->localOutL/R for the worklet to mix.
//
// Returns clap_process_status (0=error, 1=continue, 2=continue_if_not_quiet,
// 3=tail, 4=sleep).
extern "C" __attribute__((export_name("_wclapPluginProcess")))
int32_t _wclapPluginProcess(Instance *inst, int32_t frames) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end() || frames <= 0) return 0;
    auto *state = it->second;
    uint32_t f = static_cast<uint32_t>(frames);
    if (f > state->maxFrames) f = state->maxFrames;

    // Update steady_time (bytes 0-7) and frames_count (bytes 8-11) in-place.
    int64_t st = state->steady_time;
    uint32_t fc = f;
    writePlugin(inst, state->process_ptr,
                reinterpret_cast<uint8_t *>(&st), 8);
    writePlugin(inst, state->process_ptr + 8,
                reinterpret_cast<uint8_t *>(&fc), 4);
    state->steady_time += f;

    writePlugin(inst, state->in_data_ptr, state->localInL, f);
    writePlugin(inst, state->in_data_ptr + state->maxFrames * 4, state->localInR, f);
    if (f < state->maxFrames) {
        std::vector<float> zeros((state->maxFrames - f) * 2, 0.0f);
        writePlugin(inst, state->in_data_ptr + f * 4, zeros.data(), state->maxFrames - f);
        writePlugin(inst,
                    state->in_data_ptr + state->maxFrames * 4 + f * 4,
                    zeros.data() + (state->maxFrames - f),
                    state->maxFrames - f);
    }

    // Zero output buffer in plugin memory before calling process().
    std::vector<float> zeros(state->maxFrames * 2, 0.0f);
    writePlugin(inst, state->out_data_ptr, zeros.data(), state->maxFrames * 2);

    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    auto plugVal = inst->get(plugPtr);
    auto procPtr = Pointer<const wclap_process>{state->process_ptr};
    auto status  = inst->call(plugVal.process, plugPtr, procPtr);
    state->queued_input_events = 0;

    readPlugin(inst, state->out_data_ptr,
               state->localOutL, f);
    readPlugin(inst, state->out_data_ptr + state->maxFrames * 4,
               state->localOutR, f);
    if (f < 2048) {
        std::memset(state->localOutL + f, 0, (2048 - f) * sizeof(float));
        std::memset(state->localOutR + f, 0, (2048 - f) * sizeof(float));
    }

    return static_cast<int32_t>(status);
}

extern "C" __attribute__((export_name("_wclapGetInputL")))
float * _wclapGetInputL(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end()) ? it->second->localInL : nullptr;
}

extern "C" __attribute__((export_name("_wclapGetInputR")))
float * _wclapGetInputR(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end()) ? it->second->localInR : nullptr;
}

// Return host-WASM pointers to the per-slot output arrays so JS can read them
// directly via a Float32Array view of host WASM memory.
extern "C" __attribute__((export_name("_wclapGetOutputL")))
float * _wclapGetOutputL(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end()) ? it->second->localOutL : nullptr;
}

extern "C" __attribute__((export_name("_wclapGetOutputR")))
float * _wclapGetOutputR(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end()) ? it->second->localOutR : nullptr;
}

extern "C" __attribute__((export_name("_wclapDescribeParameters")))
const char * _wclapDescribeParameters(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return "";
    return it->second->parameter_json.c_str();
}

extern "C" __attribute__((export_name("_wclapEnqueueMidi2Event")))
int32_t _wclapEnqueueMidi2Event(Instance *inst, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;

    wclap_event_midi2 event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = wclap32::WCLAP_CORE_EVENT_SPACE_ID;
    event.header.type = wclap32::WCLAP_EVENT_MIDI2;
    event.header.flags = wclap32::WCLAP_EVENT_IS_LIVE;
    event.port_index = 0;
    event.data[0] = w0;
    event.data[1] = w1;
    event.data[2] = w2;
    event.data[3] = w3;
    return enqueueInputEvent(inst, it->second, &event, sizeof(event)) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapEnqueueParameterValue")))
int32_t _wclapEnqueueParameterValue(Instance *inst, uint32_t paramIndex, double value) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto *state = it->second;
    if (!state->has_params || paramIndex >= state->parameter_infos.size())
        return 0;

    wclap_event_param_value event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = wclap32::WCLAP_CORE_EVENT_SPACE_ID;
    event.header.type = wclap32::WCLAP_EVENT_PARAM_VALUE;
    event.header.flags = 0;
    event.param_id = state->parameter_infos[paramIndex].id;
    event.cookie = state->parameter_infos[paramIndex].cookie;
    event.note_id = -1;
    event.port_index = -1;
    event.channel = -1;
    event.key = -1;
    event.value = value;
    return enqueueInputEvent(inst, state, &event, sizeof(event)) ? 1 : 0;
}

// Tears down the plugin: stop_processing, deactivate, destroy, free SlotState.
extern "C" __attribute__((export_name("_wclapPluginDestroy")))
void _wclapPluginDestroy(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end()) return;
    auto *state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    auto plugVal = inst->get(plugPtr);
    inst->call(plugVal.stop_processing, plugPtr);
    inst->call(plugVal.deactivate,      plugPtr);
    inst->call(plugVal.destroy,         plugPtr);
    delete state;
    s_slots.erase(it);
}
