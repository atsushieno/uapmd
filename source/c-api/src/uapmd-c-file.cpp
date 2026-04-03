/* uapmd C API — implementation for the uapmd-file module bindings */

#include "c-api/uapmd-c-file.h"
#include <uapmd-file/IDocumentProvider.hpp>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

/* ── Cast helper ─────────────────────────────────────────────────────────── */

static uapmd::IDocumentProvider* DP(uapmd_document_provider_t h) { return reinterpret_cast<uapmd::IDocumentProvider*>(h); }

static size_t copy_string(const std::string& src, char* buf, size_t buf_size) {
    size_t required = src.size() + 1;
    if (!buf || buf_size == 0)
        return required;
    size_t to_copy = (src.size() < buf_size) ? src.size() : (buf_size - 1);
    std::memcpy(buf, src.data(), to_copy);
    buf[to_copy] = '\0';
    return to_copy;
}

/* ── C ↔ C++ conversion ─────────────────────────────────────────────────── */

static uapmd::DocumentHandle to_cpp(uapmd_document_handle_t h) {
    uapmd::DocumentHandle dh;
    if (h.id) dh.id = h.id;
    if (h.display_name) dh.display_name = h.display_name;
    if (h.mime_type) dh.mime_type = h.mime_type;
    return dh;
}

/* ── Ownership ───────────────────────────────────────────────────────────── */

static std::mutex s_dp_mutex;
static std::unordered_map<uapmd::IDocumentProvider*, std::unique_ptr<uapmd::IDocumentProvider>> s_owned_providers;

/* ═══════════════════════════════════════════════════════════════════════════
 *  IDocumentProvider
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_document_provider_t uapmd_document_provider_create() {
    auto provider = uapmd::createDocumentProvider();
    auto raw = provider.get();
    std::lock_guard lock(s_dp_mutex);
    s_owned_providers[raw] = std::move(provider);
    return reinterpret_cast<uapmd_document_provider_t>(raw);
}

void uapmd_document_provider_destroy(uapmd_document_provider_t provider) {
    if (!provider) return;
    std::lock_guard lock(s_dp_mutex);
    s_owned_providers.erase(DP(provider));
}

bool uapmd_document_provider_supports_create_file_internal() {
    return uapmd::IDocumentProvider::supportsCreateFileInternal();
}

void uapmd_document_provider_pick_open(uapmd_document_provider_t provider,
                                        const uapmd_document_filter_t* filters,
                                        uint32_t filter_count,
                                        bool allow_multiple,
                                        void* user_data,
                                        uapmd_pick_callback_t callback) {
    std::vector<uapmd::DocumentFilter> cpp_filters;
    cpp_filters.reserve(filter_count);
    for (uint32_t i = 0; i < filter_count; ++i) {
        uapmd::DocumentFilter f;
        if (filters[i].label) f.label = filters[i].label;
        for (uint32_t j = 0; j < filters[i].mime_type_count; ++j)
            f.mime_types.emplace_back(filters[i].mime_types[j]);
        for (uint32_t j = 0; j < filters[i].extension_count; ++j)
            f.extensions.emplace_back(filters[i].extensions[j]);
        cpp_filters.push_back(std::move(f));
    }

    DP(provider)->pickOpenDocuments(std::move(cpp_filters), allow_multiple,
        [callback, user_data](uapmd::DocumentPickResult result) {
            /* Build C result — allocate handles array */
            uapmd_document_pick_result_t c_result{};
            c_result.success = result.success;

            /* We need stable storage for the string fields */
            struct PickStorage {
                std::string error;
                std::vector<std::string> ids;
                std::vector<std::string> display_names;
                std::vector<std::string> mime_types;
                std::vector<uapmd_document_handle_t> handles;
            };
            auto* storage = new PickStorage;
            storage->error = result.error;
            c_result.error = storage->error.empty() ? nullptr : storage->error.c_str();

            storage->handles.resize(result.handles.size());
            storage->ids.resize(result.handles.size());
            storage->display_names.resize(result.handles.size());
            storage->mime_types.resize(result.handles.size());
            for (size_t i = 0; i < result.handles.size(); ++i) {
                storage->ids[i] = result.handles[i].id;
                storage->display_names[i] = result.handles[i].display_name;
                storage->mime_types[i] = result.handles[i].mime_type;
                storage->handles[i].id = storage->ids[i].c_str();
                storage->handles[i].display_name = storage->display_names[i].c_str();
                storage->handles[i].mime_type = storage->mime_types[i].c_str();
            }
            c_result.handle_count = static_cast<uint32_t>(storage->handles.size());
            c_result.handles = storage->handles.data();

            callback(c_result, user_data);

            /* Storage is freed by uapmd_document_pick_result_free, but the
             * callback may not call it — so we attach storage to the result
             * via a side channel. For simplicity, we let the callback own
             * the cleanup. This means the caller MUST call
             * uapmd_document_pick_result_free on the handles pointer. */
            /* Actually, since the result is passed by value and the callback
             * has it, we can't easily transfer ownership. Let's just leak-protect
             * by deleting here — the callback should have copied what it needs. */
            delete storage;
        });
}

void uapmd_document_provider_pick_save(uapmd_document_provider_t provider,
                                        const char* default_name,
                                        const uapmd_document_filter_t* filters,
                                        uint32_t filter_count,
                                        void* user_data,
                                        uapmd_pick_callback_t callback) {
    std::vector<uapmd::DocumentFilter> cpp_filters;
    cpp_filters.reserve(filter_count);
    for (uint32_t i = 0; i < filter_count; ++i) {
        uapmd::DocumentFilter f;
        if (filters[i].label) f.label = filters[i].label;
        for (uint32_t j = 0; j < filters[i].mime_type_count; ++j)
            f.mime_types.emplace_back(filters[i].mime_types[j]);
        for (uint32_t j = 0; j < filters[i].extension_count; ++j)
            f.extensions.emplace_back(filters[i].extensions[j]);
        cpp_filters.push_back(std::move(f));
    }

    DP(provider)->pickSaveDocument(default_name ? default_name : "", std::move(cpp_filters),
        [callback, user_data](uapmd::DocumentPickResult result) {
            uapmd_document_pick_result_t c_result{};
            c_result.success = result.success;

            struct PickStorage {
                std::string error;
                std::vector<std::string> ids, display_names, mime_types;
                std::vector<uapmd_document_handle_t> handles;
            };
            auto* storage = new PickStorage;
            storage->error = result.error;
            c_result.error = storage->error.empty() ? nullptr : storage->error.c_str();

            storage->handles.resize(result.handles.size());
            storage->ids.resize(result.handles.size());
            storage->display_names.resize(result.handles.size());
            storage->mime_types.resize(result.handles.size());
            for (size_t i = 0; i < result.handles.size(); ++i) {
                storage->ids[i] = result.handles[i].id;
                storage->display_names[i] = result.handles[i].display_name;
                storage->mime_types[i] = result.handles[i].mime_type;
                storage->handles[i].id = storage->ids[i].c_str();
                storage->handles[i].display_name = storage->display_names[i].c_str();
                storage->handles[i].mime_type = storage->mime_types[i].c_str();
            }
            c_result.handle_count = static_cast<uint32_t>(storage->handles.size());
            c_result.handles = storage->handles.data();

            callback(c_result, user_data);
            delete storage;
        });
}

void uapmd_document_provider_read(uapmd_document_provider_t provider,
                                   uapmd_document_handle_t handle,
                                   void* user_data,
                                   uapmd_read_callback_t callback) {
    DP(provider)->readDocument(to_cpp(handle),
        [callback, user_data](uapmd::DocumentIOResult result, std::vector<uint8_t> data) {
            uapmd_document_io_result_t c_result{};
            c_result.success = result.success;
            /* Store error string temporarily on stack — callback must copy if needed */
            c_result.error = result.error.empty() ? nullptr : result.error.c_str();
            callback(c_result, data.data(), data.size(), user_data);
        });
}

void uapmd_document_provider_write(uapmd_document_provider_t provider,
                                    uapmd_document_handle_t handle,
                                    const uint8_t* data,
                                    size_t data_size,
                                    void* user_data,
                                    uapmd_write_callback_t callback) {
    std::vector<uint8_t> buf(data, data + data_size);
    DP(provider)->writeDocument(to_cpp(handle), std::move(buf),
        [callback, user_data](uapmd::DocumentIOResult result) {
            uapmd_document_io_result_t c_result{};
            c_result.success = result.success;
            c_result.error = result.error.empty() ? nullptr : result.error.c_str();
            callback(c_result, user_data);
        });
}

void uapmd_document_provider_resolve_to_path(uapmd_document_provider_t provider,
                                              uapmd_document_handle_t handle,
                                              void* user_data,
                                              uapmd_path_callback_t callback) {
    DP(provider)->resolveToPath(to_cpp(handle),
        [callback, user_data](uapmd::DocumentIOResult result, std::filesystem::path path) {
            uapmd_document_io_result_t c_result{};
            c_result.success = result.success;
            c_result.error = result.error.empty() ? nullptr : result.error.c_str();
            auto pathStr = path.string();
            callback(c_result, pathStr.c_str(), user_data);
        });
}

void uapmd_document_provider_tick(uapmd_document_provider_t provider) {
    DP(provider)->tick();
}

size_t uapmd_document_provider_persist_handle(uapmd_document_provider_t provider,
                                               uapmd_document_handle_t handle,
                                               char* buf, size_t buf_size) {
    auto token = DP(provider)->persistHandle(to_cpp(handle));
    return copy_string(token, buf, buf_size);
}

static thread_local std::string tl_restore_id;
static thread_local std::string tl_restore_display;
static thread_local std::string tl_restore_mime;

bool uapmd_document_provider_restore_handle(uapmd_document_provider_t provider,
                                             const char* token,
                                             uapmd_document_handle_t* out) {
    auto result = DP(provider)->restoreHandle(token);
    if (!result.has_value())
        return false;
    tl_restore_id = result->id;
    tl_restore_display = result->display_name;
    tl_restore_mime = result->mime_type;
    out->id = tl_restore_id.c_str();
    out->display_name = tl_restore_display.c_str();
    out->mime_type = tl_restore_mime.c_str();
    return true;
}

void uapmd_document_pick_result_free(uapmd_document_pick_result_t* result) {
    /* Currently a no-op since we delete storage in the callback.
     * This function exists as a future-proof API entry point. */
    (void)result;
}
