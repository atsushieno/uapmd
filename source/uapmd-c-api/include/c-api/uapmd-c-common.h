/* uapmd C API — common types and macros */
#ifndef UAPMD_C_COMMON_H
#define UAPMD_C_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Export/visibility macros ──────────────────────────────────────────────── */

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef UAPMD_C_API_BUILDING
#    define UAPMD_C_EXPORT __declspec(dllexport)
#  else
#    define UAPMD_C_EXPORT __declspec(dllimport)
#  endif
#else
#  define UAPMD_C_EXPORT __attribute__((visibility("default")))
#endif

/* ── Primitive typedefs (mirror C++ CommonTypes.hpp) ──────────────────────── */

typedef int32_t  uapmd_status_t;
typedef uint32_t uapmd_ump_t;
typedef int64_t  uapmd_timestamp_t;

/* ── Opaque handles ──────────────────────────────────────────────────────── */

typedef struct uapmd_plugin_instance*       uapmd_plugin_instance_t;
typedef struct uapmd_plugin_host*           uapmd_plugin_host_t;
typedef struct uapmd_plugin_graph*          uapmd_plugin_graph_t;
typedef struct uapmd_plugin_node*           uapmd_plugin_node_t;
typedef struct uapmd_midi_io*               uapmd_midi_io_t;
typedef struct uapmd_midi_io_manager*       uapmd_midi_io_manager_t;
typedef struct uapmd_function_block*        uapmd_function_block_t;
typedef struct uapmd_function_block_mgr*    uapmd_function_block_mgr_t;
typedef struct uapmd_function_device*       uapmd_function_device_t;
typedef struct uapmd_ump_input_mapper*      uapmd_ump_input_mapper_t;
typedef struct uapmd_ump_output_mapper*     uapmd_ump_output_mapper_t;

/* ── Callback typedefs ───────────────────────────────────────────────────── */

typedef void (*uapmd_ump_receiver_t)(void* context,
                                     uapmd_ump_t* ump,
                                     size_t size_in_bytes,
                                     uapmd_timestamp_t timestamp);

/* ── String helper convention ────────────────────────────────────────────── */
/*
 * Functions returning strings follow this pattern:
 *   size_t uapmd_xxx_get_yyy(handle, char* buf, size_t buf_size);
 *
 * - If buf is NULL or buf_size is 0, returns the required buffer size
 *   (including the NUL terminator).
 * - Otherwise, copies at most buf_size-1 characters plus a NUL terminator
 *   into buf and returns the number of characters written (excluding NUL).
 */

/* ── Data structs exposed to C ───────────────────────────────────────────── */

typedef struct uapmd_parameter_named_value {
    double value;
    const char* name;  /* valid until next call that modifies parameters */
} uapmd_parameter_named_value_t;

typedef struct uapmd_parameter_metadata {
    uint32_t index;
    const char* stable_id;
    const char* name;
    const char* path;
    double default_plain_value;
    double min_plain_value;
    double max_plain_value;
    bool automatable;
    bool hidden;
    bool discrete;
    uint32_t named_values_count;
    const uapmd_parameter_named_value_t* named_values;
} uapmd_parameter_metadata_t;

typedef struct uapmd_preset_metadata {
    uint8_t bank;
    uint32_t index;
    const char* stable_id;
    const char* name;
    const char* path;
} uapmd_preset_metadata_t;

typedef enum uapmd_state_context_type {
    UAPMD_STATE_CONTEXT_REMEMBER = 0,
    UAPMD_STATE_CONTEXT_COPYABLE = 1,
    UAPMD_STATE_CONTEXT_PRESET   = 2,
    UAPMD_STATE_CONTEXT_PROJECT  = 3
} uapmd_state_context_type_t;

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_COMMON_H */
