/* uapmd C API — bindings for the uapmd-file module */
#ifndef UAPMD_C_FILE_H
#define UAPMD_C_FILE_H

#include "uapmd-c-common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handles (uapmd-file) ─────────────────────────────────────────── */

typedef struct uapmd_document_provider* uapmd_document_provider_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  DocumentHandle (value type — exposed as a C struct)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_document_handle {
    const char* id;
    const char* display_name;
    const char* mime_type;
} uapmd_document_handle_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  DocumentFilter
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_document_filter {
    const char* label;
    const char** mime_types;
    uint32_t mime_type_count;
    const char** extensions;
    uint32_t extension_count;
} uapmd_document_filter_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Result types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct uapmd_document_pick_result {
    bool success;
    uint32_t handle_count;
    uapmd_document_handle_t* handles;  /* caller must free via uapmd_document_pick_result_free */
    const char* error;
} uapmd_document_pick_result_t;

typedef struct uapmd_document_io_result {
    bool success;
    const char* error;
} uapmd_document_io_result_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*uapmd_pick_callback_t)(uapmd_document_pick_result_t result, void* user_data);
typedef void (*uapmd_read_callback_t)(uapmd_document_io_result_t result, const uint8_t* data, size_t data_size, void* user_data);
typedef void (*uapmd_write_callback_t)(uapmd_document_io_result_t result, void* user_data);
typedef void (*uapmd_path_callback_t)(uapmd_document_io_result_t result, const char* path, void* user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 *  IDocumentProvider
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_document_provider_t uapmd_document_provider_create(void);
UAPMD_C_EXPORT void uapmd_document_provider_destroy(uapmd_document_provider_t provider);

UAPMD_C_EXPORT bool uapmd_document_provider_supports_create_file_internal(void);

UAPMD_C_EXPORT void uapmd_document_provider_pick_open(uapmd_document_provider_t provider,
                                                        const uapmd_document_filter_t* filters,
                                                        uint32_t filter_count,
                                                        bool allow_multiple,
                                                        void* user_data,
                                                        uapmd_pick_callback_t callback);

UAPMD_C_EXPORT void uapmd_document_provider_pick_save(uapmd_document_provider_t provider,
                                                        const char* default_name,
                                                        const uapmd_document_filter_t* filters,
                                                        uint32_t filter_count,
                                                        void* user_data,
                                                        uapmd_pick_callback_t callback);

UAPMD_C_EXPORT void uapmd_document_provider_read(uapmd_document_provider_t provider,
                                                   uapmd_document_handle_t handle,
                                                   void* user_data,
                                                   uapmd_read_callback_t callback);

UAPMD_C_EXPORT void uapmd_document_provider_write(uapmd_document_provider_t provider,
                                                    uapmd_document_handle_t handle,
                                                    const uint8_t* data,
                                                    size_t data_size,
                                                    void* user_data,
                                                    uapmd_write_callback_t callback);

UAPMD_C_EXPORT void uapmd_document_provider_resolve_to_path(uapmd_document_provider_t provider,
                                                              uapmd_document_handle_t handle,
                                                              void* user_data,
                                                              uapmd_path_callback_t callback);

UAPMD_C_EXPORT void uapmd_document_provider_tick(uapmd_document_provider_t provider);

UAPMD_C_EXPORT size_t uapmd_document_provider_persist_handle(uapmd_document_provider_t provider,
                                                               uapmd_document_handle_t handle,
                                                               char* buf, size_t buf_size);

UAPMD_C_EXPORT bool uapmd_document_provider_restore_handle(uapmd_document_provider_t provider,
                                                             const char* token,
                                                             uapmd_document_handle_t* out);

/* Result cleanup */
UAPMD_C_EXPORT void uapmd_document_pick_result_free(uapmd_document_pick_result_t* result);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_FILE_H */
