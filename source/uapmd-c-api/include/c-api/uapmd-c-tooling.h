/* uapmd C API — bindings for remidy-tooling (PluginScanTool, PluginInstancing, etc.) */
#ifndef UAPMD_C_TOOLING_H
#define UAPMD_C_TOOLING_H

#include "uapmd-c-common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handles ─────────────────────────────────────────────────────── */

typedef struct uapmd_scan_tool*          uapmd_scan_tool_t;
typedef struct uapmd_plugin_instancing*  uapmd_plugin_instancing_t;
typedef struct uapmd_format_manager*     uapmd_format_manager_t;

/* ── Enums ──────────────────────────────────────────────────────────────── */

typedef enum uapmd_scan_mode {
    UAPMD_SCAN_MODE_IN_PROCESS = 0,
    UAPMD_SCAN_MODE_REMOTE     = 1
} uapmd_scan_mode_t;

typedef enum uapmd_instancing_state {
    UAPMD_INSTANCING_CREATED     = 0,
    UAPMD_INSTANCING_PREPARING   = 1,
    UAPMD_INSTANCING_READY       = 2,
    UAPMD_INSTANCING_ERROR       = 3,
    UAPMD_INSTANCING_TERMINATING = 4,
    UAPMD_INSTANCING_TERMINATED  = 5
} uapmd_instancing_state_t;

/* ── Data structs ───────────────────────────────────────────────────────── */

typedef struct uapmd_blocklist_entry {
    const char* id;
    const char* format;
    const char* plugin_id;
    const char* reason;
} uapmd_blocklist_entry_t;

/* ── Scan observer callbacks ────────────────────────────────────────────── */

typedef void (*uapmd_scan_started_cb_t)(uint32_t total_bundles, void* user_data);
typedef void (*uapmd_bundle_scan_cb_t)(const char* bundle_path, void* user_data);
typedef void (*uapmd_scan_completed_cb_t)(void* user_data);
typedef void (*uapmd_scan_error_cb_t)(const char* message, void* user_data);
typedef bool (*uapmd_scan_should_cancel_cb_t)(void* user_data);

typedef struct uapmd_scan_observer {
    void* user_data;
    uapmd_scan_started_cb_t      slow_scan_started;
    uapmd_bundle_scan_cb_t       bundle_scan_started;
    uapmd_bundle_scan_cb_t       bundle_scan_completed;
    uapmd_scan_completed_cb_t    slow_scan_completed;
    uapmd_scan_error_cb_t        error_occurred;
    uapmd_scan_should_cancel_cb_t should_cancel;
} uapmd_scan_observer_t;

/* Instancing callback */
typedef void (*uapmd_instancing_cb_t)(const char* error, void* user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 *  PluginScanTool
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_scan_tool_t uapmd_scan_tool_create(void);
UAPMD_C_EXPORT void              uapmd_scan_tool_destroy(uapmd_scan_tool_t tool);

/* Catalog access (returns the underlying catalog from the tool) */
UAPMD_C_EXPORT uint32_t uapmd_scan_tool_catalog_entry_count(uapmd_scan_tool_t tool);

/* Format list */
UAPMD_C_EXPORT uint32_t uapmd_scan_tool_format_count(uapmd_scan_tool_t tool);
UAPMD_C_EXPORT size_t   uapmd_scan_tool_get_format_name(uapmd_scan_tool_t tool, uint32_t index,
                                                          char* buf, size_t buf_size);

/* Cache file */
UAPMD_C_EXPORT size_t uapmd_scan_tool_get_cache_file(uapmd_scan_tool_t tool, char* buf, size_t buf_size);
UAPMD_C_EXPORT void   uapmd_scan_tool_set_cache_file(uapmd_scan_tool_t tool, const char* path);
UAPMD_C_EXPORT void   uapmd_scan_tool_save_cache(uapmd_scan_tool_t tool);
UAPMD_C_EXPORT void   uapmd_scan_tool_save_cache_to(uapmd_scan_tool_t tool, const char* path);

/* Scanning */
UAPMD_C_EXPORT void uapmd_scan_tool_perform_scanning(uapmd_scan_tool_t tool,
                                                       bool require_fast_scanning,
                                                       const uapmd_scan_observer_t* observer);

/* Blocklist */
UAPMD_C_EXPORT uint32_t uapmd_scan_tool_blocklist_count(uapmd_scan_tool_t tool);
UAPMD_C_EXPORT bool     uapmd_scan_tool_get_blocklist_entry(uapmd_scan_tool_t tool, uint32_t index,
                                                              uapmd_blocklist_entry_t* out);
UAPMD_C_EXPORT void     uapmd_scan_tool_flush_blocklist(uapmd_scan_tool_t tool);
UAPMD_C_EXPORT bool     uapmd_scan_tool_unblock_bundle(uapmd_scan_tool_t tool, const char* entry_id);
UAPMD_C_EXPORT void     uapmd_scan_tool_clear_blocklist(uapmd_scan_tool_t tool);
UAPMD_C_EXPORT void     uapmd_scan_tool_add_to_blocklist(uapmd_scan_tool_t tool,
                                                           const char* format_name,
                                                           const char* plugin_id,
                                                           const char* reason);

/* Query */
UAPMD_C_EXPORT size_t uapmd_scan_tool_last_scan_error(uapmd_scan_tool_t tool, char* buf, size_t buf_size);

/* ═══════════════════════════════════════════════════════════════════════════
 *  PluginInstancing
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_plugin_instancing_t uapmd_instancing_create(uapmd_scan_tool_t tool,
                                                                    const char* format,
                                                                    const char* plugin_id);
UAPMD_C_EXPORT void uapmd_instancing_destroy(uapmd_plugin_instancing_t inst);

UAPMD_C_EXPORT void uapmd_instancing_make_alive(uapmd_plugin_instancing_t inst,
                                                  void* user_data,
                                                  uapmd_instancing_cb_t callback);
UAPMD_C_EXPORT uapmd_instancing_state_t uapmd_instancing_state(uapmd_plugin_instancing_t inst);

/* ═══════════════════════════════════════════════════════════════════════════
 *  PluginFormatManager
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_format_manager_t uapmd_format_manager_create(void);
UAPMD_C_EXPORT void                   uapmd_format_manager_destroy(uapmd_format_manager_t mgr);
UAPMD_C_EXPORT uint32_t               uapmd_format_manager_format_count(uapmd_format_manager_t mgr);
UAPMD_C_EXPORT size_t                 uapmd_format_manager_get_format_name(uapmd_format_manager_t mgr,
                                                                             uint32_t index,
                                                                             char* buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_TOOLING_H */
