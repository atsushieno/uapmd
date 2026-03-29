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

#include <cmath>
#include <atomic>
#include <array>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
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
using wclap32::wclap_host_gui;
using wclap32::wclap_host_params;
using wclap32::wclap_host_webview;
using wclap32::wclap_input_events;
using wclap32::wclap_output_events;
using wclap32::wclap_param_info;
using wclap32::wclap_plugin;
using wclap32::wclap_plugin_entry;
using wclap32::wclap_plugin_factory;
using wclap32::wclap_plugin_gui;
using wclap32::wclap_plugin_note_ports;
using wclap32::wclap_plugin_params;
using wclap32::wclap_plugin_preset_load;
using wclap32::wclap_plugin_latency;
using wclap32::wclap_process;
using wclap32::wclap_plugin_webview;
using wclap32::wclap_plugin_state;
using wclap32::wclap_plugin_state_context;
using wclap32::wclap_istream;
using wclap32::wclap_ostream;

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
host_get_ext(void *, Pointer<const wclap_host>, Pointer<const char>);
static void host_req_restart (void *, Pointer<const wclap_host>) {}
static void host_req_process (void *, Pointer<const wclap_host>) {}
static void host_req_callback(void *, Pointer<const wclap_host>);
static void host_gui_resize_hints_changed(void *, Pointer<const wclap_host>) {}
static bool host_gui_request_resize(void *, Pointer<const wclap_host>, uint32_t, uint32_t);
static bool host_gui_request_show(void *, Pointer<const wclap_host>) { return true; }
static bool host_gui_request_hide(void *, Pointer<const wclap_host>) { return true; }
static void host_gui_closed(void *, Pointer<const wclap_host>, bool) {}
static void host_params_rescan(void *, Pointer<const wclap_host>, uint32_t);
static void host_params_clear(void *, Pointer<const wclap_host>, wclap32::wclap_id, uint32_t) {}
static void host_params_request_flush(void *, Pointer<const wclap_host>);
static bool host_webview_send(void *, Pointer<const wclap_host>, Pointer<const void>, uint32_t);

static uint32_t in_events_size(void *ctx, Pointer<const wclap_input_events>);
static Pointer<const wclap_event_header> in_events_get(void *ctx, Pointer<const wclap_input_events>, uint32_t index);
static int64_t state_stream_read(void *ctx, Pointer<const wclap_istream>, Pointer<void>, uint64_t size);
static int64_t state_stream_write(void *ctx, Pointer<const wclap_ostream>, Pointer<const void>, uint64_t size);

static bool
out_events_try_push(void *, Pointer<const wclap_output_events>,
                    Pointer<const wclap_event_header>);

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

extern "C" __attribute__((export_name("_wclapHostGuiResizeHintsChanged")))
void _wclapHostGuiResizeHintsChanged(void *ctx, Pointer<const wclap_host> host) {
    host_gui_resize_hints_changed(ctx, host);
}

extern "C" __attribute__((export_name("_wclapHostGuiRequestResize")))
bool _wclapHostGuiRequestResize(void *ctx, Pointer<const wclap_host> host, uint32_t width, uint32_t height) {
    return host_gui_request_resize(ctx, host, width, height);
}

extern "C" __attribute__((export_name("_wclapHostGuiRequestShow")))
bool _wclapHostGuiRequestShow(void *ctx, Pointer<const wclap_host> host) {
    return host_gui_request_show(ctx, host);
}

extern "C" __attribute__((export_name("_wclapHostGuiRequestHide")))
bool _wclapHostGuiRequestHide(void *ctx, Pointer<const wclap_host> host) {
    return host_gui_request_hide(ctx, host);
}

extern "C" __attribute__((export_name("_wclapHostGuiClosed")))
void _wclapHostGuiClosed(void *ctx, Pointer<const wclap_host> host, bool wasDestroyed) {
    host_gui_closed(ctx, host, wasDestroyed);
}

extern "C" __attribute__((export_name("_wclapHostParamsRescan")))
void _wclapHostParamsRescan(void *ctx, Pointer<const wclap_host> host, uint32_t flags) {
    host_params_rescan(ctx, host, flags);
}

extern "C" __attribute__((export_name("_wclapHostParamsClear")))
void _wclapHostParamsClear(void *ctx, Pointer<const wclap_host> host, wclap32::wclap_id paramId, uint32_t flags) {
    host_params_clear(ctx, host, paramId, flags);
}

extern "C" __attribute__((export_name("_wclapHostParamsRequestFlush")))
void _wclapHostParamsRequestFlush(void *ctx, Pointer<const wclap_host> host) {
    host_params_request_flush(ctx, host);
}

extern "C" __attribute__((export_name("_wclapHostWebviewSend")))
bool _wclapHostWebviewSend(void *ctx, Pointer<const wclap_host> host, Pointer<const void> data, uint32_t size) {
    return host_webview_send(ctx, host, data, size);
}

extern "C" __attribute__((export_name("_wclapStateStreamRead")))
int64_t _wclapStateStreamRead(void *ctx, Pointer<const wclap_istream> stream, Pointer<void> buffer, uint64_t size) {
    return state_stream_read(ctx, stream, buffer, size);
}

extern "C" __attribute__((export_name("_wclapStateStreamWrite")))
int64_t _wclapStateStreamWrite(void *ctx, Pointer<const wclap_ostream> stream, Pointer<const void> buffer, uint64_t size) {
    return state_stream_write(ctx, stream, buffer, size);
}

// ── Per-slot state ─────────────────────────────────────────────────────────
// One SlotState is created by _wclapPluginSetup and destroyed by
// _wclapPluginDestroy. All uint32_t members are byte-offsets into the
// *plugin* WASM linear memory; localOut* are in *host* WASM heap memory.

struct SlotState {
    static constexpr uint32_t kMaxInputEvents = 64;
    static constexpr uint32_t kInputEventStride = 64;

    Instance* owner_inst  = nullptr;
    uint32_t plugin_ptr    = 0;
    uint32_t host_gui_ptr  = 0;
    uint32_t host_params_ptr = 0;
    uint32_t host_webview_ptr = 0;
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
    bool has_gui = false;
    bool has_webview = false;
    bool callback_requested = false;
    bool flush_requested = false;
    bool flushing_params = false;
    bool parameter_values_dirty = false;
    bool ui_created = false;
    bool ui_visible = false;
    bool ui_can_resize = false;
    bool ui_resize_pending = false;
    bool has_event_inputs = false;
    bool has_event_outputs = false;
    bool has_state = false;
    bool has_state_context = false;
    bool has_preset_load = false;
    uint32_t latency_in_samples = 0;
    uint32_t ui_width = 800;
    uint32_t ui_height = 600;
    wclap_plugin_params params{};
    wclap_plugin_gui gui{};
    wclap_plugin_webview webview{};
    wclap_plugin_state state_ext{};
    wclap_plugin_state_context state_context_ext{};
    std::vector<wclap_param_info> parameter_infos{};
    std::vector<double> parameter_values{};
    std::string parameter_json{};
    std::vector<std::pair<uint32_t, double>> pending_parameter_updates{};
    std::string parameter_updates_json{};
    std::string ui_json{};
    std::string ui_uri{};
    std::string ui_resize_json{};
    std::vector<uint8_t> ui_outgoing_message{};
    std::deque<std::vector<uint8_t>> ui_outgoing_messages{};
    std::array<char, 8192> ui_incoming_buffer{};
    float localInL[2048]{};
    float localInR[2048]{};
    float localOutL[2048]{};
    float localOutR[2048]{};
    uint32_t last_param_write_index = 0;
    double last_param_write_value = 0.0;
    std::string selected_plugin_id{};
    uint32_t state_istream_ptr = 0;
    uint32_t state_ostream_ptr = 0;
    uint32_t state_context_type = 0;
    uint32_t state_transfer_offset = 0;
    std::vector<uint8_t> state_transfer{};
    std::string formatted_value_text{};
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

static bool
out_events_try_push(void *ctx, Pointer<const wclap_output_events>,
                    Pointer<const wclap_event_header> eventHeaderPtr)
{
    auto* state = static_cast<SlotState*>(ctx);
    if (!state || !state->owner_inst || !eventHeaderPtr.wasmPointer)
        return true;

    wclap_event_header header{};
    state->owner_inst->getArray(Pointer<wclap_event_header>{eventHeaderPtr.wasmPointer}, &header, 1);
    switch (header.type) {
        case wclap32::WCLAP_EVENT_PARAM_VALUE:
            state->parameter_values_dirty = true;
            if (header.size >= sizeof(wclap_event_param_value)) {
                wclap_event_param_value event{};
                state->owner_inst->getArray(Pointer<wclap_event_param_value>{eventHeaderPtr.wasmPointer}, &event, 1);
                for (uint32_t index = 0; index < state->parameter_infos.size(); ++index) {
                    if (state->parameter_infos[index].id == event.param_id) {
                        state->pending_parameter_updates.emplace_back(index, event.value);
                        break;
                    }
                }
            }
            break;
        case wclap32::WCLAP_EVENT_PARAM_MOD:
        case wclap32::WCLAP_EVENT_PARAM_GESTURE_BEGIN:
        case wclap32::WCLAP_EVENT_PARAM_GESTURE_END:
            state->parameter_values_dirty = true;
            break;
        default:
            break;
    }
    return true;
}

static std::unordered_map<Instance *, SlotState *> s_slots;

struct UiObservedParamUpdate {
    uint32_t index{};
    double value{};
};

static std::string debugPreviewBytes(const uint8_t* bytes, uint32_t size);

static bool cborReadLength(const uint8_t*& ptr, const uint8_t* end, uint8_t addl, uint64_t& out) {
    if (addl < 24) {
        out = addl;
        return true;
    }
    auto readN = [&](uint32_t count) -> bool {
        if (ptr + count > end)
            return false;
        out = 0;
        for (uint32_t i = 0; i < count; ++i)
            out = (out << 8) | *ptr++;
        return true;
    };
    switch (addl) {
        case 24: return readN(1);
        case 25: return readN(2);
        case 26: return readN(4);
        case 27: return readN(8);
        default: return false;
    }
}

static bool cborReadText(const uint8_t*& ptr, const uint8_t* end, std::string& out) {
    if (ptr >= end)
        return false;
    const auto initial = *ptr++;
    if ((initial >> 5) != 3)
        return false;
    uint64_t length = 0;
    if (!cborReadLength(ptr, end, initial & 0x1f, length) || ptr + length > end)
        return false;
    out.assign(reinterpret_cast<const char*>(ptr), reinterpret_cast<const char*>(ptr + length));
    ptr += length;
    return true;
}

static bool cborReadDouble(const uint8_t*& ptr, const uint8_t* end, double& out) {
    if (ptr >= end)
        return false;
    const auto initial = *ptr++;
    const auto major = initial >> 5;
    const auto addl = initial & 0x1f;
    if (major == 0 || major == 1) {
        uint64_t value = 0;
        if (!cborReadLength(ptr, end, addl, value))
            return false;
        out = major == 0 ? static_cast<double>(value) : -1.0 - static_cast<double>(value);
        return true;
    }
    if (major != 7)
        return false;
    if (addl == 27) {
        if (ptr + 8 > end)
            return false;
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits = (bits << 8) | *ptr++;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    if (addl == 26) {
        if (ptr + 4 > end)
            return false;
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i)
            bits = (bits << 8) | *ptr++;
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        out = value;
        return true;
    }
    if (addl == 20) {
        out = 0.0;
        return true;
    }
    if (addl == 21) {
        out = 1.0;
        return true;
    }
    return false;
}

static bool cborReadBool(const uint8_t*& ptr, const uint8_t* end, bool& out) {
    if (ptr >= end)
        return false;
    const auto initial = *ptr++;
    if (initial == 0xf4) {
        out = false;
        return true;
    }
    if (initial == 0xf5) {
        out = true;
        return true;
    }
    return false;
}

static bool cborSkip(const uint8_t*& ptr, const uint8_t* end);

static bool cborSkipMap(const uint8_t*& ptr, const uint8_t* end, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        if (!cborSkip(ptr, end) || !cborSkip(ptr, end))
            return false;
    }
    return true;
}

static bool cborSkipArray(const uint8_t*& ptr, const uint8_t* end, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        if (!cborSkip(ptr, end))
            return false;
    }
    return true;
}

static bool cborSkip(const uint8_t*& ptr, const uint8_t* end) {
    if (ptr >= end)
        return false;
    const auto initial = *ptr++;
    const auto major = initial >> 5;
    uint64_t length = 0;
    switch (major) {
        case 0:
        case 1:
            return cborReadLength(ptr, end, initial & 0x1f, length);
        case 2:
        case 3:
            return cborReadLength(ptr, end, initial & 0x1f, length) && ptr + length <= end ? (ptr += length, true) : false;
        case 4:
            return cborReadLength(ptr, end, initial & 0x1f, length) && cborSkipArray(ptr, end, length);
        case 5:
            return cborReadLength(ptr, end, initial & 0x1f, length) && cborSkipMap(ptr, end, length);
        case 7:
            switch (initial & 0x1f) {
                case 20:
                case 21:
                case 22:
                case 23:
                    return true;
                case 25:
                    return ptr + 2 <= end ? (ptr += 2, true) : false;
                case 26:
                    return ptr + 4 <= end ? (ptr += 4, true) : false;
                case 27:
                    return ptr + 8 <= end ? (ptr += 8, true) : false;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static bool decodeObservedUiParamMerges(const uint8_t* data,
                                        uint32_t size,
                                        const SlotState* state,
                                        std::vector<UiObservedParamUpdate>& updates) {
    if (!data || !state || size == 0)
        return false;
    const uint8_t* ptr = data;
    const uint8_t* end = data + size;
    if (ptr >= end)
        return false;
    const auto initial = *ptr++;
    if ((initial >> 5) != 5)
        return false;
    uint64_t topCount = 0;
    if (!cborReadLength(ptr, end, initial & 0x1f, topCount))
        return false;

    bool foundAny = false;
    for (uint64_t i = 0; i < topCount; ++i) {
        std::string key;
        if (!cborReadText(ptr, end, key))
            return false;
        if (ptr >= end)
            return false;
        const auto valueInitial = *ptr++;
        if ((valueInitial >> 5) != 5) {
            if (!cborSkip(ptr, end))
                return false;
            continue;
        }
        uint64_t nestedCount = 0;
        if (!cborReadLength(ptr, end, valueInitial & 0x1f, nestedCount))
            return false;
        bool hasRangeUnit = false;
        double rangeUnit = 0.0;
        bool hasValue = false;
        double value = 0.0;
        for (uint64_t j = 0; j < nestedCount; ++j) {
            std::string nestedKey;
            if (!cborReadText(ptr, end, nestedKey))
                return false;
            if (nestedKey == "rangeUnit") {
                hasRangeUnit = cborReadDouble(ptr, end, rangeUnit);
                if (!hasRangeUnit)
                    return false;
            } else if (nestedKey == "value") {
                hasValue = cborReadDouble(ptr, end, value);
                if (!hasValue)
                    return false;
            } else if (nestedKey == "gesture") {
                bool ignored = false;
                if (!cborReadBool(ptr, end, ignored))
                    return false;
            } else if (!cborSkip(ptr, end)) {
                return false;
            }
        }

        auto it = std::find_if(state->parameter_infos.begin(), state->parameter_infos.end(),
                               [&](const wclap_param_info& info) { return key == info.name; });
        if (it == state->parameter_infos.end())
            continue;
        const auto index = static_cast<uint32_t>(std::distance(state->parameter_infos.begin(), it));
        double plainValue = state->parameter_values.size() > index ? state->parameter_values[index] : it->default_value;
        if (hasValue) {
            plainValue = value;
        } else if (hasRangeUnit) {
            const auto normalized = std::clamp(rangeUnit, 0.0, 1.0);
            plainValue = it->min_value + (it->max_value - it->min_value) * normalized;
        } else {
            continue;
        }
        updates.push_back({index, plainValue});
        foundAny = true;
    }
    return foundAny;
}

static void cborAppendTypeAndLength(std::vector<uint8_t>& out, uint8_t major, uint64_t length) {
    if (length < 24) {
        out.push_back(static_cast<uint8_t>((major << 5) | length));
        return;
    }
    if (length <= std::numeric_limits<uint8_t>::max()) {
        out.push_back(static_cast<uint8_t>((major << 5) | 24));
        out.push_back(static_cast<uint8_t>(length));
        return;
    }
    if (length <= std::numeric_limits<uint16_t>::max()) {
        out.push_back(static_cast<uint8_t>((major << 5) | 25));
        out.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(length & 0xff));
        return;
    }
    out.push_back(static_cast<uint8_t>((major << 5) | 26));
    out.push_back(static_cast<uint8_t>((length >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((length >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(length & 0xff));
}

static void cborAppendText(std::vector<uint8_t>& out, const std::string& text) {
    cborAppendTypeAndLength(out, 3, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

static void cborAppendBool(std::vector<uint8_t>& out, bool value) {
    out.push_back(value ? 0xf5 : 0xf4);
}

static void cborAppendDouble(std::vector<uint8_t>& out, double value) {
    out.push_back(0xfb);
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>((bits >> shift) & 0xff));
}

static std::pair<std::string, std::string> splitValueText(const std::string& text) {
    const auto pos = text.rfind(' ');
    if (pos == std::string::npos)
        return {text, ""};
    return {text.substr(0, pos), text.substr(pos + 1)};
}

static std::string formatParameterValueText(Instance* inst,
                                            SlotState* state,
                                            uint32_t index,
                                            double value) {
    if (!inst || !state || !state->has_params || index >= state->parameter_infos.size())
        return {};
    auto textPtr = inst->malloc32(256);
    if (!textPtr.wasmPointer)
        return {};
    const auto& info = state->parameter_infos[index];
    if (!inst->call(state->params.value_to_text,
                    Pointer<const wclap_plugin>{state->plugin_ptr},
                    info.id,
                    value,
                    Pointer<char>{textPtr.wasmPointer},
                    255u)) {
        return {};
    }
    return inst->getString(Pointer<const char>{textPtr.wasmPointer}, 255);
}

static std::vector<uint8_t> encodeSyntheticUiMerge(Instance* inst,
                                                   SlotState* state,
                                                   Pointer<const wclap_plugin> plugPtr,
                                                   const std::vector<UiObservedParamUpdate>& updates) {
    std::vector<uint8_t> out;
    if (!inst || !state || updates.empty())
        return out;
    cborAppendTypeAndLength(out, 5, updates.size());
    auto textPtr = inst->malloc32(256);
    for (const auto& update : updates) {
        if (update.index >= state->parameter_infos.size())
            continue;
        const auto& info = state->parameter_infos[update.index];
        cborAppendText(out, info.name);
        const bool stepped = (info.flags & wclap32::WCLAP_PARAM_IS_STEPPED) != 0;
        cborAppendTypeAndLength(out, 5, stepped ? 2 : 4);
        if (stepped) {
            cborAppendText(out, "value");
            cborAppendDouble(out, update.value);
        } else {
            const double denom = info.max_value - info.min_value;
            const double rangeUnit = denom == 0.0 ? 0.0 : std::clamp((update.value - info.min_value) / denom, 0.0, 1.0);
            cborAppendText(out, "rangeUnit");
            cborAppendDouble(out, rangeUnit);
            cborAppendText(out, "gesture");
            cborAppendBool(out, false);
        }
        std::string textValue;
        if (textPtr.wasmPointer &&
            inst->call(state->params.value_to_text,
                       plugPtr,
                       info.id,
                       update.value,
                       Pointer<char>{textPtr.wasmPointer},
                       255u)) {
            textValue = inst->getString(Pointer<const char>{textPtr.wasmPointer}, 255);
        }
        auto [text, units] = splitValueText(textValue);
        cborAppendText(out, "text");
        cborAppendText(out, text);
        if (!stepped) {
            cborAppendText(out, "textUnits");
            cborAppendText(out, units);
        }
    }
    return out;
}

static bool applyObservedUiParamUpdates(SlotState* state,
                                        const std::vector<UiObservedParamUpdate>& updates) {
    if (!state || updates.empty())
        return false;
    if (state->parameter_values.size() != state->parameter_infos.size())
        state->parameter_values.resize(state->parameter_infos.size(), 0.0);
    bool changed = false;
    for (const auto& update : updates) {
        if (update.index >= state->parameter_infos.size())
            continue;
        if (std::abs(state->parameter_values[update.index] - update.value) <= 1.0e-9)
            continue;
        state->parameter_values[update.index] = update.value;
        state->pending_parameter_updates.emplace_back(update.index, update.value);
        changed = true;
    }
    return changed;
}

static void enqueueSyntheticUiMerge(Instance* inst,
                                    SlotState* state,
                                    Pointer<const wclap_plugin> plugPtr,
                                    const std::vector<UiObservedParamUpdate>& updates) {
    if (!state || updates.empty())
        return;
    auto payload = encodeSyntheticUiMerge(inst, state, plugPtr, updates);
    if (payload.empty())
        return;
    state->ui_outgoing_messages.emplace_back(std::move(payload));
}

static void rebuildUiJson(SlotState* state) {
    if (!state)
        return;
    std::ostringstream json;
    json << "{"
         << "\"hasUi\":" << ((state->has_webview || state->has_gui) ? "true" : "false")
         << ",\"hasGui\":" << (state->has_gui ? "true" : "false")
         << ",\"hasWebview\":" << (state->has_webview ? "true" : "false")
         << ",\"canResize\":" << (state->ui_can_resize ? "true" : "false")
         << ",\"width\":" << state->ui_width
         << ",\"height\":" << state->ui_height
         << "}";
    state->ui_json = json.str();
}

static std::string buildCapabilitiesJson(const SlotState* state) {
    std::ostringstream json;
    json << "{"
         << "\"hasEventInputs\":" << (state && state->has_event_inputs ? "true" : "false")
         << ",\"hasEventOutputs\":" << (state && state->has_event_outputs ? "true" : "false")
         << ",\"hasState\":" << (state && state->has_state ? "true" : "false")
         << ",\"hasPresetLoad\":" << (state && state->has_preset_load ? "true" : "false")
         << ",\"latencyInSamples\":" << (state ? state->latency_in_samples : 0)
         << "}";
    return json.str();
}

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

static int64_t state_stream_read(void *ctx,
                                 Pointer<const wclap_istream>,
                                 Pointer<void> buffer,
                                 uint64_t size) {
    auto *state = static_cast<SlotState *>(ctx);
    if (!state || !state->owner_inst || !buffer.wasmPointer || size == 0)
        return 0;
    if (state->state_transfer_offset >= state->state_transfer.size())
        return 0;
    const auto remaining = state->state_transfer.size() - state->state_transfer_offset;
    const auto bytesToCopy = static_cast<uint32_t>(std::min<uint64_t>(size, remaining));
    writePlugin(state->owner_inst,
                buffer.wasmPointer,
                state->state_transfer.data() + state->state_transfer_offset,
                bytesToCopy);
    state->state_transfer_offset += bytesToCopy;
    return static_cast<int64_t>(bytesToCopy);
}

static int64_t state_stream_write(void *ctx,
                                  Pointer<const wclap_ostream>,
                                  Pointer<const void> buffer,
                                  uint64_t size) {
    auto *state = static_cast<SlotState *>(ctx);
    if (!state || !state->owner_inst || !buffer.wasmPointer || size == 0)
        return 0;
    const auto previousSize = state->state_transfer.size();
    const auto bytesToCopy = static_cast<uint32_t>(size);
    state->state_transfer.resize(previousSize + bytesToCopy);
    readPlugin(state->owner_inst,
               buffer.wasmPointer,
               state->state_transfer.data() + previousSize,
               bytesToCopy);
    state->state_transfer_offset += bytesToCopy;
    return static_cast<int64_t>(bytesToCopy);
}

static uint32_t remidyStateContextToWclapStateContext(uint32_t stateContextType) {
    switch (stateContextType) {
        case 0:
        case 1:
            return wclap32::WCLAP_STATE_CONTEXT_FOR_DUPLICATE;
        case 2:
            return wclap32::WCLAP_STATE_CONTEXT_FOR_PRESET;
        default:
            return wclap32::WCLAP_STATE_CONTEXT_FOR_PROJECT;
    }
}

// Allocate size bytes in plugin memory, write data, return the address.
static uint32_t allocInPlugin(Instance *inst, const void *data, uint32_t size) {
    auto p = inst->malloc32(size);
    if (p.wasmPointer == 0) return 0;
    writePlugin(inst, p.wasmPointer,
                reinterpret_cast<const uint8_t *>(data), size);
    return p.wasmPointer;
}

static bool pluginSupportsWebviewUi(Instance* inst, SlotState* state, Pointer<const wclap_plugin> plugPtr) {
    if (!inst || !state || !state->has_gui)
        return false;
    auto apiPtr = allocInPlugin(inst,
        wclap32::WCLAP_WINDOW_API_WEBVIEW,
        sizeof(wclap32::WCLAP_WINDOW_API_WEBVIEW));
    if (!apiPtr)
        return false;
    return inst->call(state->gui.is_api_supported,
                      plugPtr,
                      Pointer<const char>{apiPtr},
                      false);
}

static void updateUiSizeFromPlugin(Instance* inst, SlotState* state, Pointer<const wclap_plugin> plugPtr) {
    if (!inst || !state || !state->has_gui || !state->ui_created)
        return;
    auto widthPtr = inst->malloc32(sizeof(uint32_t));
    auto heightPtr = inst->malloc32(sizeof(uint32_t));
    if (!widthPtr.wasmPointer || !heightPtr.wasmPointer)
        return;
    if (inst->call(state->gui.get_size, plugPtr,
                   Pointer<uint32_t>{widthPtr.wasmPointer},
                   Pointer<uint32_t>{heightPtr.wasmPointer})) {
        uint32_t width = state->ui_width;
        uint32_t height = state->ui_height;
        readPlugin(inst, widthPtr.wasmPointer, &width, 1);
        readPlugin(inst, heightPtr.wasmPointer, &height, 1);
        if (width > 0)
            state->ui_width = width;
        if (height > 0)
            state->ui_height = height;
    }
}

static bool prepareUi(Instance* inst, SlotState* state, uint32_t width, uint32_t height) {
    if (!inst || !state)
        return false;

    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (state->has_gui) {
        if (!state->ui_created) {
            auto apiPtr = allocInPlugin(inst,
                wclap32::WCLAP_WINDOW_API_WEBVIEW,
                sizeof(wclap32::WCLAP_WINDOW_API_WEBVIEW));
            if (!apiPtr)
                return false;
            if (!inst->call(state->gui.create, plugPtr, Pointer<const char>{apiPtr}, false))
                return false;
            state->ui_created = true;
            state->ui_visible = false;
        }

        state->ui_can_resize = inst->call(state->gui.can_resize, plugPtr);
        if (width > 0 && height > 0) {
            uint32_t adjustedWidth = width;
            uint32_t adjustedHeight = height;
            auto widthPtr = inst->malloc32(sizeof(uint32_t));
            auto heightPtr = inst->malloc32(sizeof(uint32_t));
            if (widthPtr.wasmPointer && heightPtr.wasmPointer) {
                writePlugin(inst, widthPtr.wasmPointer, &adjustedWidth, 1);
                writePlugin(inst, heightPtr.wasmPointer, &adjustedHeight, 1);
                if (inst->call(state->gui.adjust_size,
                               plugPtr,
                               Pointer<uint32_t>{widthPtr.wasmPointer},
                               Pointer<uint32_t>{heightPtr.wasmPointer})) {
                    readPlugin(inst, widthPtr.wasmPointer, &adjustedWidth, 1);
                    readPlugin(inst, heightPtr.wasmPointer, &adjustedHeight, 1);
                }
                if (adjustedWidth > 0 && adjustedHeight > 0)
                    inst->call(state->gui.set_size, plugPtr, adjustedWidth, adjustedHeight);
            }
        }
        updateUiSizeFromPlugin(inst, state, plugPtr);
    }

    if (state->has_webview) {
        auto uriPtr = inst->malloc32(4096);
        if (!uriPtr.wasmPointer)
            return false;
        auto written = inst->call(state->webview.get_uri,
                                  plugPtr,
                                  Pointer<char>{uriPtr.wasmPointer},
                                  4096u);
        if (written > 0)
            state->ui_uri = inst->getString(Pointer<const char>{uriPtr.wasmPointer}, 4096);
    }

    rebuildUiJson(state);
    return state->has_webview || state->has_gui;
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

static Pointer<const void>
host_get_ext(void *ctx, Pointer<const wclap_host>, Pointer<const char> id) {
    auto* state = static_cast<SlotState*>(ctx);
    if (!state || !state->owner_inst || !id.wasmPointer)
        return {0};
    auto extId = state->owner_inst->getString(id, 128);
    if (extId == wclap32::WCLAP_EXT_GUI && state->host_gui_ptr)
        return {state->host_gui_ptr};
    if (extId == wclap32::WCLAP_EXT_PARAMS && state->host_params_ptr)
        return {state->host_params_ptr};
    if (extId == wclap32::WCLAP_EXT_WEBVIEW && state->host_webview_ptr)
        return {state->host_webview_ptr};
    return {0};
}

static void host_req_callback(void *ctx, Pointer<const wclap_host>) {
    auto* state = static_cast<SlotState*>(ctx);
    if (state)
        state->callback_requested = true;
}

static bool host_gui_request_resize(void *ctx, Pointer<const wclap_host>, uint32_t width, uint32_t height) {
    auto* state = static_cast<SlotState*>(ctx);
    if (!state)
        return false;
    state->ui_width = width;
    state->ui_height = height;
    state->ui_resize_pending = true;
    std::ostringstream json;
    json << "{"
         << "\"width\":" << width
         << ",\"height\":" << height
         << ",\"canResize\":" << (state->ui_can_resize ? "true" : "false")
         << "}";
    state->ui_resize_json = json.str();
    rebuildUiJson(state);
    return true;
}

static bool host_webview_send(void *ctx, Pointer<const wclap_host>, Pointer<const void> data, uint32_t size) {
    auto* state = static_cast<SlotState*>(ctx);
    if (!state || !state->owner_inst || !data.wasmPointer || size == 0)
        return false;
    std::vector<uint8_t> payload(size);
    readPlugin(state->owner_inst, data.wasmPointer, payload.data(), size);
    state->ui_outgoing_messages.emplace_back(std::move(payload));
    return true;
}

static void host_params_rescan(void *ctx, Pointer<const wclap_host>, uint32_t) {
    auto* state = static_cast<SlotState*>(ctx);
    if (state)
        state->parameter_values_dirty = true;
}

static void host_params_request_flush(void *ctx, Pointer<const wclap_host>) {
    auto* state = static_cast<SlotState*>(ctx);
    if (state)
        state->flush_requested = true;
}

static void rebuildParameterJson(Instance *inst, SlotState *state, Pointer<const wclap_plugin> plugPtr) {
    if (!state || !state->has_params)
        return;

    state->parameter_infos.clear();
    state->parameter_values.clear();
    state->parameter_json = "[]";

    auto count = inst->call(state->params.count, plugPtr);
    if (count == 0)
        return;

    auto infoPtr = inst->malloc32(sizeof(wclap_param_info));
    if (!infoPtr.wasmPointer)
        return;

    state->parameter_infos.reserve(count);
    state->parameter_values.reserve(count);
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
        state->parameter_values.emplace_back(currentValue);

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
             << ",\"automatablePerKey\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_AUTOMATABLE_PER_KEY) ? "true" : "false")
             << ",\"automatablePerChannel\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL) ? "true" : "false")
             << ",\"automatablePerPort\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_AUTOMATABLE_PER_PORT) ? "true" : "false")
             << ",\"modulatablePerKey\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_MODULATABLE_PER_KEY) ? "true" : "false")
             << ",\"modulatablePerChannel\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_MODULATABLE_PER_CHANNEL) ? "true" : "false")
             << ",\"modulatablePerPort\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_MODULATABLE_PER_PORT) ? "true" : "false")
             << ",\"modulatablePerNoteId\":" << ((info.flags & wclap32::WCLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) ? "true" : "false")
             << "}";
    }
    json << "]";
    state->parameter_json = json.str();
}

static void syncParameterValueUpdates(Instance *inst, SlotState *state, Pointer<const wclap_plugin> plugPtr) {
    if (!inst || !state || !state->has_params)
        return;
    if (state->parameter_infos.empty())
        return;

    if (state->parameter_values.size() != state->parameter_infos.size())
        state->parameter_values.assign(state->parameter_infos.size(), 0.0);

    auto valuePtr = inst->malloc32(sizeof(double));
    if (!valuePtr.wasmPointer)
        return;

    for (uint32_t index = 0; index < state->parameter_infos.size(); ++index) {
        const auto& info = state->parameter_infos[index];
        double currentValue = state->parameter_values[index];
        if (!inst->call(state->params.get_value, plugPtr, info.id, Pointer<double>{valuePtr.wasmPointer}))
            continue;
        readPlugin(inst, valuePtr.wasmPointer, &currentValue, 1);
        if (std::abs(currentValue - state->parameter_values[index]) > 1.0e-9) {
            state->parameter_values[index] = currentValue;
            state->pending_parameter_updates.emplace_back(index, currentValue);
        }
    }
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

static void serviceMainThreadCallbacks(Instance *inst, SlotState *state, bool callOnce = false) {
    if (!inst || !state)
        return;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    auto plugVal = inst->get(plugPtr);
    if (!plugVal.on_main_thread.wasmPointer)
        return;
    if (callOnce) {
        inst->call(plugVal.on_main_thread, plugPtr);
    }
    if (!state->callback_requested)
        return;
    for (int guard = 0; guard < 8 && state->callback_requested; ++guard) {
        state->callback_requested = false;
        inst->call(plugVal.on_main_thread, plugPtr);
    }
}

static bool flushParameterChanges(Instance *inst, SlotState *state, bool force) {
    if (!inst || !state || !state->has_params || state->flushing_params)
        return false;
    if (!force && !state->flush_requested && state->queued_input_events == 0)
        return false;

    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    state->flushing_params = true;
    state->flush_requested = false;
    state->parameter_values_dirty = false;
    inst->call(state->params.flush,
               plugPtr,
               Pointer<const wclap_input_events>{state->in_events_ptr},
               Pointer<const wclap_output_events>{state->out_events_ptr});
    state->queued_input_events = 0;
    rebuildParameterJson(inst, state, plugPtr);
    state->flushing_params = false;
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
    state->owner_inst = inst;
    if (auto path = inst->path(); path) {
        std::string_view pluginPath{path};
        static constexpr std::string_view marker = "#plugin=";
        if (auto pos = pluginPath.find(marker); pos != std::string_view::npos)
            state->selected_plugin_id = std::string{pluginPath.substr(pos + marker.size())};
    }

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

    wclap_host_gui hostGui{};
    hostGui.resize_hints_changed = inst->registerHost32<void,
        Pointer<const wclap_host>>(state, host_gui_resize_hints_changed);
    hostGui.request_resize = inst->registerHost32<bool,
        Pointer<const wclap_host>, uint32_t, uint32_t>(state, host_gui_request_resize);
    hostGui.request_show = inst->registerHost32<bool,
        Pointer<const wclap_host>>(state, host_gui_request_show);
    hostGui.request_hide = inst->registerHost32<bool,
        Pointer<const wclap_host>>(state, host_gui_request_hide);
    hostGui.closed = inst->registerHost32<void,
        Pointer<const wclap_host>, bool>(state, host_gui_closed);

    wclap_host_webview hostWebview{};
    hostWebview.send = inst->registerHost32<bool,
        Pointer<const wclap_host>, Pointer<const void>, uint32_t>(state, host_webview_send);

    wclap_host_params hostParams{};
    hostParams.rescan = inst->registerHost32<void,
        Pointer<const wclap_host>, uint32_t>(state, host_params_rescan);
    hostParams.clear = inst->registerHost32<void,
        Pointer<const wclap_host>, wclap32::wclap_id, uint32_t>(state, host_params_clear);
    hostParams.request_flush = inst->registerHost32<void,
        Pointer<const wclap_host>>(state, host_params_request_flush);

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

    wclap_istream stateInput{};
    stateInput.ctx = {0};
    stateInput.read = inst->registerHost32<int64_t,
        Pointer<const wclap_istream>, Pointer<void>, uint64_t>(state, state_stream_read);

    wclap_ostream stateOutput{};
    stateOutput.ctx = {0};
    stateOutput.write = inst->registerHost32<int64_t,
        Pointer<const wclap_ostream>, Pointer<const void>, uint64_t>(state, state_stream_write);

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

    std::string pluginId = state->selected_plugin_id;
    if (pluginId.empty()) {
        auto descPtr = inst->call(factVal.get_plugin_descriptor, factPtr, 0u);
        if (descPtr.wasmPointer == 0) {
            std::cerr << "[wclap] get_plugin_descriptor(0) returned null\n";
            delete state;
            return 0;
        }
        auto descVal = inst->get(descPtr);
        pluginId = inst->getString(descVal.id, 256);
    } else {
        bool found = false;
        for (uint32_t index = 0; index < pluginCount; ++index) {
            auto descPtr = inst->call(factVal.get_plugin_descriptor, factPtr, index);
            if (descPtr.wasmPointer == 0)
                continue;
            auto descVal = inst->get(descPtr);
            if (pluginId == inst->getString(descVal.id, 256)) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "[wclap] plugin id not found in factory: " << pluginId << "\n";
            delete state;
            return 0;
        }
    }

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

    state->host_gui_ptr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(&hostGui),
        sizeof(hostGui));
    state->host_params_ptr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(&hostParams),
        sizeof(hostParams));
    state->host_webview_ptr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(&hostWebview),
        sizeof(hostWebview));

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

    uint32_t stateInputPtr = allocInPlugin(inst,
                             reinterpret_cast<const uint8_t *>(&stateInput), sizeof(stateInput));
    if (!stateInputPtr) {
        delete state;
        return 0;
    }

    uint32_t stateOutputPtr = allocInPlugin(inst,
                              reinterpret_cast<const uint8_t *>(&stateOutput), sizeof(stateOutput));
    if (!stateOutputPtr) {
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
    state->state_istream_ptr = stateInputPtr;
    state->state_ostream_ptr = stateOutputPtr;
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

    static const char kGuiExtId[] = "clap.gui";
    auto guiIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(kGuiExtId),
        sizeof(kGuiExtId));
    if (guiIdPtr) {
        auto guiVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{guiIdPtr});
        if (guiVoid.wasmPointer != 0) {
            state->has_gui = true;
            state->gui = inst->get(Pointer<const wclap_plugin_gui>{guiVoid.wasmPointer});
        }
    }

    static const char kWebviewExtId[] = "clap.webview/3";
    auto webviewIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(kWebviewExtId),
        sizeof(kWebviewExtId));
    if (webviewIdPtr) {
        auto webviewVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{webviewIdPtr});
        if (webviewVoid.wasmPointer != 0) {
            state->has_webview = true;
            state->webview = inst->get(Pointer<const wclap_plugin_webview>{webviewVoid.wasmPointer});
        }
    }
    static const char kNotePortsExtId[] = "clap.note-ports";
    auto notePortsIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(kNotePortsExtId),
        sizeof(kNotePortsExtId));
    if (notePortsIdPtr) {
        auto notePortsVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{notePortsIdPtr});
        if (notePortsVoid.wasmPointer != 0) {
            auto notePorts = inst->get(Pointer<const wclap_plugin_note_ports>{notePortsVoid.wasmPointer});
            state->has_event_inputs = inst->call(notePorts.count, plugPtr, true) > 0;
            state->has_event_outputs = inst->call(notePorts.count, plugPtr, false) > 0;
        }
    }
    static const char kStateExtId[] = "clap.state";
    auto stateIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(kStateExtId),
        sizeof(kStateExtId));
    if (stateIdPtr) {
        auto stateVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{stateIdPtr});
        state->has_state = stateVoid.wasmPointer != 0;
        if (state->has_state)
            state->state_ext = inst->get(Pointer<const wclap_plugin_state>{stateVoid.wasmPointer});
    }
    if (!state->has_state) {
        static const char kStateContextExtId[] = "clap.state-context/2";
        auto stateContextIdPtr = allocInPlugin(inst,
            reinterpret_cast<const uint8_t*>(kStateContextExtId),
            sizeof(kStateContextExtId));
        if (stateContextIdPtr) {
            auto stateContextVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{stateContextIdPtr});
            state->has_state_context = stateContextVoid.wasmPointer != 0;
            state->has_state = state->has_state_context;
            if (state->has_state_context)
                state->state_context_ext = inst->get(Pointer<const wclap_plugin_state_context>{stateContextVoid.wasmPointer});
        }
    }
    static const char kPresetLoadExtId[] = "clap.preset-load/2";
    auto presetLoadIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(kPresetLoadExtId),
        sizeof(kPresetLoadExtId));
    if (presetLoadIdPtr) {
        auto presetLoadVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{presetLoadIdPtr});
        state->has_preset_load = presetLoadVoid.wasmPointer != 0;
    }
    static const char kLatencyExtId[] = "clap.latency";
    auto latencyIdPtr = allocInPlugin(inst,
        reinterpret_cast<const uint8_t*>(kLatencyExtId),
        sizeof(kLatencyExtId));
    if (latencyIdPtr) {
        auto latencyVoid = inst->call(plugVal.get_extension, plugPtr, Pointer<const char>{latencyIdPtr});
        if (latencyVoid.wasmPointer != 0) {
            auto latency = inst->get(Pointer<const wclap_plugin_latency>{latencyVoid.wasmPointer});
            state->latency_in_samples = inst->call(latency.get, plugPtr);
        }
    }
    rebuildUiJson(state);
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
    serviceMainThreadCallbacks(inst, state, true);
    if (state->parameter_values_dirty) {
        syncParameterValueUpdates(inst, state, plugPtr);
        state->parameter_values_dirty = false;
    }

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

extern "C" __attribute__((export_name("_wclapFormatParameterValue")))
const char * _wclapFormatParameterValue(Instance *inst, uint32_t paramIndex, double value) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return "";
    auto* state = it->second;
    state->formatted_value_text = formatParameterValueText(inst, state, paramIndex, value);
    return state->formatted_value_text.c_str();
}

extern "C" __attribute__((export_name("_wclapHasParameterUpdates")))
int32_t _wclapHasParameterUpdates(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end() && !it->second->pending_parameter_updates.empty()) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapTakeParameterUpdates")))
const char * _wclapTakeParameterUpdates(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end() || it->second->pending_parameter_updates.empty())
        return "[]";
    auto* state = it->second;
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& update : state->pending_parameter_updates) {
        if (!first)
            json << ",";
        first = false;
        json << "{\"index\":" << update.first << ",\"value\":" << update.second << "}";
    }
    json << "]";
    state->parameter_updates_json = json.str();
    state->pending_parameter_updates.clear();
    return state->parameter_updates_json.c_str();
}

extern "C" __attribute__((export_name("_wclapDescribeUi")))
const char * _wclapDescribeUi(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return "";
    auto* state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (state->has_gui && pluginSupportsWebviewUi(inst, state, plugPtr)) {
        prepareUi(inst, state, state->ui_width, state->ui_height);
        if (state->ui_created) {
            inst->call(state->gui.destroy, plugPtr);
            state->ui_created = false;
            state->ui_visible = false;
        }
    } else {
        rebuildUiJson(state);
    }
    return state->ui_json.c_str();
}

extern "C" __attribute__((export_name("_wclapDescribeCapabilities")))
const char * _wclapDescribeCapabilities(Instance *inst) {
    static std::string json = "{}";
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return json.c_str();
    json = buildCapabilitiesJson(it->second);
    return json.c_str();
}

extern "C" __attribute__((export_name("_wclapStateSave")))
int32_t _wclapStateSave(Instance *inst, uint32_t stateContextType) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto *state = it->second;
    if (!state->has_state)
        return 0;

    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    state->state_transfer.clear();
    state->state_transfer_offset = 0;
    state->state_context_type = remidyStateContextToWclapStateContext(stateContextType);

    serviceMainThreadCallbacks(inst, state, true);
    if (state->has_params) {
        auto flushed = flushParameterChanges(inst, state, true);
        (void) flushed;
        serviceMainThreadCallbacks(inst, state, true);
        syncParameterValueUpdates(inst, state, plugPtr);
        state->parameter_values_dirty = false;
    }

    if (state->has_state_context)
        return inst->call(state->state_context_ext.save,
                          plugPtr,
                          Pointer<const wclap_ostream>{state->state_ostream_ptr},
                          state->state_context_type) ? 1 : 0;

    return inst->call(state->state_ext.save,
                      plugPtr,
                      Pointer<const wclap_ostream>{state->state_ostream_ptr}) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapStateGetSize")))
uint32_t _wclapStateGetSize(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    return static_cast<uint32_t>(it->second->state_transfer.size());
}

extern "C" __attribute__((export_name("_wclapStateGetData")))
const uint8_t * _wclapStateGetData(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end() || it->second->state_transfer.empty())
        return nullptr;
    return it->second->state_transfer.data();
}

extern "C" __attribute__((export_name("_wclapStatePrepareLoad")))
uint8_t * _wclapStatePrepareLoad(Instance *inst, uint32_t size) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return nullptr;
    auto *state = it->second;
    state->state_transfer.resize(size);
    state->state_transfer_offset = 0;
    return size == 0 ? nullptr : state->state_transfer.data();
}

extern "C" __attribute__((export_name("_wclapStateLoad")))
int32_t _wclapStateLoad(Instance *inst, uint32_t size, uint32_t stateContextType) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto *state = it->second;
    if (!state->has_state)
        return 0;

    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (size < state->state_transfer.size())
        state->state_transfer.resize(size);
    state->state_transfer_offset = 0;
    state->state_context_type = remidyStateContextToWclapStateContext(stateContextType);

    const bool loaded = state->has_state_context
        ? inst->call(state->state_context_ext.load,
                     plugPtr,
                     Pointer<const wclap_istream>{state->state_istream_ptr},
                     state->state_context_type)
        : inst->call(state->state_ext.load,
                     plugPtr,
                     Pointer<const wclap_istream>{state->state_istream_ptr});
    if (!loaded)
        return 0;

    serviceMainThreadCallbacks(inst, state, true);
    if (state->has_params) {
        syncParameterValueUpdates(inst, state, plugPtr);
        state->parameter_values_dirty = false;
    }
    return 1;
}

extern "C" __attribute__((export_name("_wclapDescribePlugins")))
const char * _wclapDescribePlugins(Instance *inst) {
    static std::string json = "[]";
    if (!inst)
        return json.c_str();

    if (!inst->init()) {
        std::cerr << "[wclap] inst->init() failed in _wclapDescribePlugins\n";
        return json.c_str();
    }

    static const uint8_t kRoot[] = {'/', '\0'};
    uint32_t pathPtr = allocInPlugin(inst, kRoot, sizeof(kRoot));
    if (!pathPtr)
        return json.c_str();

    auto entryVal = inst->get(inst->entry32);
    if (entryVal.init.wasmPointer)
        inst->call(entryVal.init, Pointer<const char>{pathPtr});

    static const char kFactoryId[] = "clap.plugin-factory";
    uint32_t fidPtr = allocInPlugin(inst,
                                    reinterpret_cast<const uint8_t *>(kFactoryId),
                                    sizeof(kFactoryId));
    if (!fidPtr)
        return json.c_str();

    auto factVoid = inst->call(entryVal.get_factory, Pointer<const char>{fidPtr});
    if (factVoid.wasmPointer == 0)
        return json.c_str();

    auto factPtr = Pointer<const wclap_plugin_factory>{factVoid.wasmPointer};
    auto factVal = inst->get(factPtr);
    uint32_t pluginCount = inst->call(factVal.get_plugin_count, factPtr);

    auto appendJsonString = [](std::ostringstream& out, const std::string& value) {
        out << '"';
        for (char c : value) {
            switch (c) {
                case '\\': out << "\\\\"; break;
                case '"': out << "\\\""; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default: out << c; break;
            }
        }
        out << '"';
    };

    std::ostringstream result;
    result << "[";
    bool first = true;
    for (uint32_t index = 0; index < pluginCount; ++index) {
        auto descPtr = inst->call(factVal.get_plugin_descriptor, factPtr, index);
        if (descPtr.wasmPointer == 0)
            continue;
        auto descVal = inst->get(descPtr);
        auto pluginId = inst->getString(descVal.id, 256);
        auto name = inst->getString(descVal.name, 256);
        auto vendor = inst->getString(descVal.vendor, 256);
        auto url = inst->getString(descVal.url, 512);
        if (!first)
            result << ",";
        first = false;
        result << "{\"id\":";
        appendJsonString(result, pluginId);
        result << ",\"name\":";
        appendJsonString(result, name);
        result << ",\"vendor\":";
        appendJsonString(result, vendor);
        result << ",\"url\":";
        appendJsonString(result, url);
        result << "}";
    }
    result << "]";
    json = result.str();
    return json.c_str();
}

extern "C" __attribute__((export_name("_wclapUiCreate")))
int32_t _wclapUiCreate(Instance *inst, uint32_t width, uint32_t height) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto* state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (state->has_gui && !pluginSupportsWebviewUi(inst, state, plugPtr))
        return 0;
    if (!prepareUi(inst, state, width, height))
        return 0;
    serviceMainThreadCallbacks(inst, state, true);
    return 1;
}

extern "C" __attribute__((export_name("_wclapUiShow")))
int32_t _wclapUiShow(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto* state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (state->has_gui && state->ui_created && !inst->call(state->gui.show, plugPtr))
        return 0;
    state->ui_visible = true;
    serviceMainThreadCallbacks(inst, state, true);
    return 1;
}

extern "C" __attribute__((export_name("_wclapUiHide")))
void _wclapUiHide(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return;
    auto* state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (state->has_gui && state->ui_created)
        inst->call(state->gui.hide, plugPtr);
    state->ui_visible = false;
    serviceMainThreadCallbacks(inst, state, true);
}

extern "C" __attribute__((export_name("_wclapUiDestroy")))
void _wclapUiDestroy(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return;
    auto* state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (state->has_gui && state->ui_created)
        inst->call(state->gui.destroy, plugPtr);
    state->ui_created = false;
    state->ui_visible = false;
    serviceMainThreadCallbacks(inst, state, true);
}

extern "C" __attribute__((export_name("_wclapUiSetSize")))
int32_t _wclapUiSetSize(Instance *inst, uint32_t width, uint32_t height) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto* state = it->second;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    if (!state->has_gui || !state->ui_created)
        return 0;
    uint32_t adjustedWidth = width;
    uint32_t adjustedHeight = height;
    auto widthPtr = inst->malloc32(sizeof(uint32_t));
    auto heightPtr = inst->malloc32(sizeof(uint32_t));
    if (widthPtr.wasmPointer && heightPtr.wasmPointer) {
        writePlugin(inst, widthPtr.wasmPointer, &adjustedWidth, 1);
        writePlugin(inst, heightPtr.wasmPointer, &adjustedHeight, 1);
        if (inst->call(state->gui.adjust_size,
                       plugPtr,
                       Pointer<uint32_t>{widthPtr.wasmPointer},
                       Pointer<uint32_t>{heightPtr.wasmPointer})) {
            readPlugin(inst, widthPtr.wasmPointer, &adjustedWidth, 1);
            readPlugin(inst, heightPtr.wasmPointer, &adjustedHeight, 1);
        }
    }
    if (!inst->call(state->gui.set_size, plugPtr, adjustedWidth, adjustedHeight))
        return 0;
    state->ui_width = adjustedWidth;
    state->ui_height = adjustedHeight;
    rebuildUiJson(state);
    serviceMainThreadCallbacks(inst, state, true);
    return 1;
}

extern "C" __attribute__((export_name("_wclapUiGetWidth")))
uint32_t _wclapUiGetWidth(Instance *inst) {
    auto it = s_slots.find(inst);
    return it == s_slots.end() ? 0 : it->second->ui_width;
}

extern "C" __attribute__((export_name("_wclapUiGetHeight")))
uint32_t _wclapUiGetHeight(Instance *inst) {
    auto it = s_slots.find(inst);
    return it == s_slots.end() ? 0 : it->second->ui_height;
}

extern "C" __attribute__((export_name("_wclapUiCanResize")))
int32_t _wclapUiCanResize(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end() && it->second->ui_can_resize) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapUiGetUri")))
const char * _wclapUiGetUri(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return "";
    return it->second->ui_uri.c_str();
}

extern "C" __attribute__((export_name("_wclapUiHasPendingResize")))
int32_t _wclapUiHasPendingResize(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end() && it->second->ui_resize_pending) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapUiTakeResizeRequest")))
const char * _wclapUiTakeResizeRequest(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end() || !it->second->ui_resize_pending)
        return "";
    it->second->ui_resize_pending = false;
    return it->second->ui_resize_json.c_str();
}

extern "C" __attribute__((export_name("_wclapUiMessageBufferPtr")))
char * _wclapUiMessageBufferPtr(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return nullptr;
    return it->second->ui_incoming_buffer.data();
}

extern "C" __attribute__((export_name("_wclapUiMessageBufferCapacity")))
uint32_t _wclapUiMessageBufferCapacity(Instance *inst) {
    auto it = s_slots.find(inst);
    return it == s_slots.end() ? 0 : static_cast<uint32_t>(it->second->ui_incoming_buffer.size());
}

extern "C" __attribute__((export_name("_wclapUiReceiveMessage")))
int32_t _wclapUiReceiveMessage(Instance *inst, uint32_t size) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto* state = it->second;
    if (!state->has_webview || size == 0 || size > state->ui_incoming_buffer.size())
        return 0;
    auto plugPtr = Pointer<const wclap_plugin>{state->plugin_ptr};
    auto dataPtr = allocInPlugin(inst, state->ui_incoming_buffer.data(), size);
    if (!dataPtr)
        return 0;
    std::vector<UiObservedParamUpdate> observedUiUpdates{};
    const auto hadObservedUiUpdates = decodeObservedUiParamMerges(
        reinterpret_cast<const uint8_t*>(state->ui_incoming_buffer.data()),
        size,
        state,
        observedUiUpdates);
    const auto pendingUpdatesBefore = state->pending_parameter_updates.size();
    const auto outgoingMessagesBefore = state->ui_outgoing_messages.size();
    if (!inst->call(state->webview.receive, plugPtr, Pointer<const void>{dataPtr}, size))
        return 0;

    // Webview-originated edits may mutate plugin state immediately without
    // waiting for the normal audio process() cadence. Give the plugin a main
    // thread turn, service any requested params flush, then resync parameter
    // values so both host state and any outgoing UI merges stay current.
    serviceMainThreadCallbacks(inst, state, true);
    auto flushed = flushParameterChanges(inst, state, false);
    serviceMainThreadCallbacks(inst, state, true);
    (void) flushed;
    syncParameterValueUpdates(inst, state, plugPtr);
    state->parameter_values_dirty = false;
    if (hadObservedUiUpdates && state->pending_parameter_updates.size() == pendingUpdatesBefore) {
        const auto applied = applyObservedUiParamUpdates(state, observedUiUpdates);
        if (applied && state->ui_outgoing_messages.size() == outgoingMessagesBefore)
            enqueueSyntheticUiMerge(inst, state, plugPtr, observedUiUpdates);
    }
    return 1;
}

extern "C" __attribute__((export_name("_wclapFlushParameters")))
int32_t _wclapFlushParameters(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto* state = it->second;
    if (!state->has_params)
        return 0;
    const auto outgoingMessagesBefore = state->ui_outgoing_messages.size();
    auto flushed = flushParameterChanges(inst, state, true);
    serviceMainThreadCallbacks(inst, state, true);
    syncParameterValueUpdates(inst, state, Pointer<const wclap_plugin>{state->plugin_ptr});
    state->parameter_values_dirty = false;
    if (state->ui_outgoing_messages.size() == outgoingMessagesBefore &&
        state->last_param_write_index < state->parameter_infos.size()) {
        const auto paramIndex = state->last_param_write_index;
        const auto value = state->last_param_write_value;
        if (state->parameter_values.size() != state->parameter_infos.size())
            state->parameter_values.resize(state->parameter_infos.size(), 0.0);
        state->parameter_values[paramIndex] = value;
        enqueueSyntheticUiMerge(inst,
                                state,
                                Pointer<const wclap_plugin>{state->plugin_ptr},
                                {{paramIndex, value}});
    }
    return flushed ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapUiHasOutgoingMessage")))
int32_t _wclapUiHasOutgoingMessage(Instance *inst) {
    auto it = s_slots.find(inst);
    return (it != s_slots.end() && !it->second->ui_outgoing_messages.empty()) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapUiDequeueOutgoingMessage")))
uint32_t _wclapUiDequeueOutgoingMessage(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end() || it->second->ui_outgoing_messages.empty())
        return 0;
    auto* state = it->second;
    state->ui_outgoing_message = std::move(state->ui_outgoing_messages.front());
    state->ui_outgoing_messages.pop_front();
    return static_cast<uint32_t>(state->ui_outgoing_message.size());
}

extern "C" __attribute__((export_name("_wclapUiGetOutgoingMessageData")))
const uint8_t * _wclapUiGetOutgoingMessageData(Instance *inst) {
    auto it = s_slots.find(inst);
    if (it == s_slots.end() || it->second->ui_outgoing_message.empty())
        return nullptr;
    return it->second->ui_outgoing_message.data();
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
    state->last_param_write_index = paramIndex;
    state->last_param_write_value = value;

    wclap_event_param_value event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = wclap32::WCLAP_CORE_EVENT_SPACE_ID;
    event.header.type = wclap32::WCLAP_EVENT_PARAM_VALUE;
    event.header.flags = wclap32::WCLAP_EVENT_IS_LIVE;
    event.param_id = state->parameter_infos[paramIndex].id;
    // Match the upstream browser host: host-side param writes identify the
    // parameter by ID and do not round-trip the plugin cookie here.
    event.cookie = {0};
    event.note_id = -1;
    event.port_index = -1;
    event.channel = -1;
    event.key = -1;
    event.value = value;
    return enqueueInputEvent(inst, state, &event, sizeof(event)) ? 1 : 0;
}

extern "C" __attribute__((export_name("_wclapEnqueuePerNoteParameterValue")))
int32_t _wclapEnqueuePerNoteParameterValue(
    Instance *inst,
    uint32_t paramIndex,
    double value,
    uint32_t group,
    uint32_t channel,
    uint32_t note)
{
    auto it = s_slots.find(inst);
    if (it == s_slots.end())
        return 0;
    auto *state = it->second;
    if (!state->has_params || paramIndex >= state->parameter_infos.size())
        return 0;
    state->last_param_write_index = paramIndex;
    state->last_param_write_value = value;

    wclap_event_param_value event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = wclap32::WCLAP_CORE_EVENT_SPACE_ID;
    event.header.type = wclap32::WCLAP_EVENT_PARAM_VALUE;
    event.header.flags = wclap32::WCLAP_EVENT_IS_LIVE;
    event.param_id = state->parameter_infos[paramIndex].id;
    event.cookie = {0};
    event.note_id = -1;
    event.port_index = static_cast<int16_t>(group);
    event.channel = static_cast<int16_t>(channel);
    event.key = static_cast<int16_t>(note);
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
