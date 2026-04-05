/* uapmd C API — implementation for remidy-tooling bindings */

#include "c-api/uapmd-c-tooling.h"
#include <remidy-tooling/remidy-tooling.hpp>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

/* ── Cast helpers ─────────────────────────────────────────────────────────── */

static remidy_tooling::PluginScanTool*   PST(uapmd_scan_tool_t h) { return reinterpret_cast<remidy_tooling::PluginScanTool*>(h); }
static remidy_tooling::PluginInstancing* PI(uapmd_plugin_instancing_t h) { return reinterpret_cast<remidy_tooling::PluginInstancing*>(h); }
static remidy_tooling::PluginFormatManager* PFM(uapmd_format_manager_t h) { return reinterpret_cast<remidy_tooling::PluginFormatManager*>(h); }

/* ── copy_string ──────────────────────────────────────────────────────────── */

static size_t copy_string(const std::string& src, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return src.size() + 1;
    size_t n = src.size() < buf_size - 1 ? src.size() : buf_size - 1;
    std::memcpy(buf, src.data(), n);
    buf[n] = '\0';
    return n;
}

/* ── Ownership registries ─────────────────────────────────────────────────── */

static std::mutex s_scan_tool_mutex;
static std::unordered_map<remidy_tooling::PluginScanTool*, std::unique_ptr<remidy_tooling::PluginScanTool>> s_owned_scan_tools;

static std::mutex s_instancing_mutex;
static std::unordered_map<remidy_tooling::PluginInstancing*, std::unique_ptr<remidy_tooling::PluginInstancing>> s_owned_instancings;

static std::mutex s_fmt_mgr_mutex;
static std::unordered_map<remidy_tooling::PluginFormatManager*, std::unique_ptr<remidy_tooling::PluginFormatManager>> s_owned_fmt_mgrs;

/* ── Thread-local storage ─────────────────────────────────────────────────── */

static thread_local std::vector<remidy::PluginFormat*> tl_formats;
static thread_local std::vector<remidy_tooling::BlocklistEntry> tl_blocklist;

/* ═══════════════════════════════════════════════════════════════════════════
 *  PluginScanTool
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_scan_tool_t uapmd_scan_tool_create() {
    auto tool = remidy_tooling::PluginScanTool::create();
    auto raw = tool.get();
    std::lock_guard lock(s_scan_tool_mutex);
    s_owned_scan_tools[raw] = std::move(tool);
    return reinterpret_cast<uapmd_scan_tool_t>(raw);
}

void uapmd_scan_tool_destroy(uapmd_scan_tool_t tool) {
    if (!tool) return;
    std::lock_guard lock(s_scan_tool_mutex);
    s_owned_scan_tools.erase(PST(tool));
}

uint32_t uapmd_scan_tool_catalog_entry_count(uapmd_scan_tool_t tool) {
    return static_cast<uint32_t>(PST(tool)->catalog().getPlugins().size());
}

uint32_t uapmd_scan_tool_format_count(uapmd_scan_tool_t tool) {
    tl_formats = PST(tool)->formats();
    return static_cast<uint32_t>(tl_formats.size());
}

size_t uapmd_scan_tool_get_format_name(uapmd_scan_tool_t tool, uint32_t index, char* buf, size_t buf_size) {
    if (tl_formats.empty())
        tl_formats = PST(tool)->formats();
    if (index >= tl_formats.size())
        return 0;
    return copy_string(tl_formats[index]->name(), buf, buf_size);
}

size_t uapmd_scan_tool_get_cache_file(uapmd_scan_tool_t tool, char* buf, size_t buf_size) {
    return copy_string(PST(tool)->pluginListCacheFile().string(), buf, buf_size);
}

void uapmd_scan_tool_set_cache_file(uapmd_scan_tool_t tool, const char* path) {
    PST(tool)->pluginListCacheFile() = path ? path : "";
}

void uapmd_scan_tool_save_cache(uapmd_scan_tool_t tool) {
    PST(tool)->savePluginListCache();
}

void uapmd_scan_tool_save_cache_to(uapmd_scan_tool_t tool, const char* path) {
    auto p = std::filesystem::path(path ? path : "");
    PST(tool)->savePluginListCache(p);
}

void uapmd_scan_tool_perform_scanning(uapmd_scan_tool_t tool,
                                       bool require_fast_scanning,
                                       const uapmd_scan_observer_t* observer) {
    remidy_tooling::PluginScanObserver obs{};
    void* ud = observer ? observer->user_data : nullptr;

    if (observer && observer->slow_scan_started)
        obs.slowScanStarted = [cb = observer->slow_scan_started, ud](uint32_t total) { cb(total, ud); };
    if (observer && observer->bundle_scan_started)
        obs.bundleScanStarted = [cb = observer->bundle_scan_started, ud](const std::filesystem::path& p) { cb(p.string().c_str(), ud); };
    if (observer && observer->bundle_scan_completed)
        obs.bundleScanCompleted = [cb = observer->bundle_scan_completed, ud](const std::filesystem::path& p) { cb(p.string().c_str(), ud); };
    if (observer && observer->slow_scan_completed)
        obs.slowScanCompleted = [cb = observer->slow_scan_completed, ud]() { cb(ud); };
    if (observer && observer->error_occurred)
        obs.errorOccurred = [cb = observer->error_occurred, ud](const std::string& msg) { cb(msg.c_str(), ud); };
    if (observer && observer->should_cancel)
        obs.shouldCancel = [cb = observer->should_cancel, ud]() -> bool { return cb(ud); };

    PST(tool)->performPluginScanning(require_fast_scanning,
        remidy_tooling::ScanMode::InProcess, false, 0.0, &obs);
}

uint32_t uapmd_scan_tool_blocklist_count(uapmd_scan_tool_t tool) {
    tl_blocklist = PST(tool)->blocklistEntries();
    return static_cast<uint32_t>(tl_blocklist.size());
}

bool uapmd_scan_tool_get_blocklist_entry(uapmd_scan_tool_t tool, uint32_t index, uapmd_blocklist_entry_t* out) {
    if (tl_blocklist.empty())
        tl_blocklist = PST(tool)->blocklistEntries();
    if (index >= tl_blocklist.size()) return false;
    auto& e = tl_blocklist[index];
    out->id = e.id.c_str();
    out->format = e.format.c_str();
    out->plugin_id = e.pluginId.c_str();
    out->reason = e.reason.c_str();
    return true;
}

void uapmd_scan_tool_flush_blocklist(uapmd_scan_tool_t tool) { PST(tool)->flushBlocklist(); }

bool uapmd_scan_tool_unblock_bundle(uapmd_scan_tool_t tool, const char* entry_id) {
    return PST(tool)->unblockBundle(entry_id ? entry_id : "");
}

void uapmd_scan_tool_clear_blocklist(uapmd_scan_tool_t tool) { PST(tool)->clearBlocklist(); }

void uapmd_scan_tool_add_to_blocklist(uapmd_scan_tool_t tool,
                                       const char* format_name,
                                       const char* plugin_id,
                                       const char* reason) {
    PST(tool)->addToBlocklist(format_name ? format_name : "",
                              plugin_id ? plugin_id : "",
                              reason ? reason : "");
}

size_t uapmd_scan_tool_last_scan_error(uapmd_scan_tool_t tool, char* buf, size_t buf_size) {
    return copy_string(PST(tool)->lastScanError(), buf, buf_size);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PluginInstancing
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_plugin_instancing_t uapmd_instancing_create(uapmd_scan_tool_t tool,
                                                    const char* format,
                                                    const char* plugin_id) {
    auto inst = std::make_unique<remidy_tooling::PluginInstancing>(
        *PST(tool), format ? format : "", plugin_id ? plugin_id : "");
    auto raw = inst.get();
    std::lock_guard lock(s_instancing_mutex);
    s_owned_instancings[raw] = std::move(inst);
    return reinterpret_cast<uapmd_plugin_instancing_t>(raw);
}

void uapmd_instancing_destroy(uapmd_plugin_instancing_t inst) {
    if (!inst) return;
    std::lock_guard lock(s_instancing_mutex);
    s_owned_instancings.erase(PI(inst));
}

void uapmd_instancing_make_alive(uapmd_plugin_instancing_t inst,
                                  void* user_data,
                                  uapmd_instancing_cb_t callback) {
    PI(inst)->makeAlive([callback, user_data](std::string error) {
        if (callback)
            callback(error.empty() ? nullptr : error.c_str(), user_data);
    });
}

uapmd_instancing_state_t uapmd_instancing_state(uapmd_plugin_instancing_t inst) {
    return static_cast<uapmd_instancing_state_t>(PI(inst)->instancingState().load());
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PluginFormatManager
 * ═══════════════════════════════════════════════════════════════════════════ */

static thread_local std::vector<remidy::PluginFormat*> tl_mgr_formats;

uapmd_format_manager_t uapmd_format_manager_create() {
    auto mgr = std::make_unique<remidy_tooling::PluginFormatManager>();
    auto raw = mgr.get();
    std::lock_guard lock(s_fmt_mgr_mutex);
    s_owned_fmt_mgrs[raw] = std::move(mgr);
    return reinterpret_cast<uapmd_format_manager_t>(raw);
}

void uapmd_format_manager_destroy(uapmd_format_manager_t mgr) {
    if (!mgr) return;
    std::lock_guard lock(s_fmt_mgr_mutex);
    s_owned_fmt_mgrs.erase(PFM(mgr));
}

uint32_t uapmd_format_manager_format_count(uapmd_format_manager_t mgr) {
    tl_mgr_formats = PFM(mgr)->formats();
    return static_cast<uint32_t>(tl_mgr_formats.size());
}

size_t uapmd_format_manager_get_format_name(uapmd_format_manager_t mgr, uint32_t index,
                                              char* buf, size_t buf_size) {
    if (tl_mgr_formats.empty())
        tl_mgr_formats = PFM(mgr)->formats();
    if (index >= tl_mgr_formats.size())
        return 0;
    return copy_string(tl_mgr_formats[index]->name(), buf, buf_size);
}
