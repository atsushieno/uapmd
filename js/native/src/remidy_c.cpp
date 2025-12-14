#include "../include/remidy_c.h"
#include "remidy/remidy.hpp"
#include "remidy-tooling/PluginScanTool.hpp"
#include "remidy-gui/remidy-gui.hpp"
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <thread>
#include <iostream>
#include <future>
#include <type_traits>
#include <utility>

using namespace remidy;
using namespace remidy_tooling;

// Node.js EventLoop implementation
namespace {
    class NodeJSEventLoop : public remidy::EventLoop {
    private:
        void (*enqueue_callback_)(RemidyMainThreadTask task, void* user_data, void* context);
        void* context_;
        std::mutex task_mutex_;
        struct TaskWrapper {
            std::function<void()> func;
        };
        std::thread::id main_thread_id_{};

    public:
        NodeJSEventLoop(void (*enqueue_callback)(RemidyMainThreadTask, void*, void*), void* context)
            : enqueue_callback_(enqueue_callback),
              context_(context),
              main_thread_id_(std::this_thread::get_id()) {}

    protected:
        void initializeOnUIThreadImpl() override {
            main_thread_id_ = std::this_thread::get_id();
        }

        bool runningOnMainThreadImpl() override {
            if (main_thread_id_ == std::thread::id{}) {
                return false;
            }
            return std::this_thread::get_id() == main_thread_id_;
        }

        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            // Create a wrapper that will be passed to Node.js
            auto* wrapper = new TaskWrapper{std::move(func)};

            // Call the JavaScript callback to schedule this on Node.js event loop
            enqueue_callback_(
                [](void* user_data) {
                    auto* wrapper = static_cast<TaskWrapper*>(user_data);
                    remidy::gui::GLContextGuard guard;
                    wrapper->func();
                    delete wrapper;
                },
                wrapper,
                context_
            );
        }

        void startImpl() override {
            // Node.js event loop is already running
        }

        void stopImpl() override {
            // We don't control Node.js event loop
        }
    };

    NodeJSEventLoop* nodejs_event_loop = nullptr;
}

struct RemidyContainerWindow {
    std::unique_ptr<remidy::gui::ContainerWindow> window;
    RemidyContainerWindowCloseCallback closeCallback{nullptr};
    void* closeContext{nullptr};
};

struct RemidyGLContextGuard {
    std::unique_ptr<remidy::gui::GLContextGuard> guard;
};

template <typename Fn>
auto runOnMainThreadSync(Fn&& fn) -> decltype(fn()) {
    using Result = decltype(fn());
    std::promise<Result> promise;
    EventLoop::runTaskOnMainThread([fn = std::forward<Fn>(fn), &promise]() mutable {
        try {
            if constexpr (std::is_void_v<Result>) {
                fn();
                promise.set_value();
            } else {
                promise.set_value(fn());
            }
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    if constexpr (std::is_void_v<Result>) {
        promise.get_future().get();
    } else {
        return promise.get_future().get();
    }
}

// Helper to convert C++ StatusCode to C enum
static RemidyStatusCode to_c_status(StatusCode status) {
    switch (status) {
        case StatusCode::OK: return REMIDY_OK;
        case StatusCode::NOT_IMPLEMENTED: return REMIDY_NOT_SUPPORTED;
        case StatusCode::INVALID_PARAMETER_OPERATION: return REMIDY_INVALID_PARAMETER;
        default: return REMIDY_ERROR;
    }
}

// ========== PluginCatalog API ==========

extern "C" {

RemidyPluginCatalog* remidy_catalog_create() {
    return reinterpret_cast<RemidyPluginCatalog*>(new PluginCatalog());
}

void remidy_catalog_destroy(RemidyPluginCatalog* catalog) {
    delete reinterpret_cast<PluginCatalog*>(catalog);
}

void remidy_catalog_clear(RemidyPluginCatalog* catalog) {
    reinterpret_cast<PluginCatalog*>(catalog)->clear();
}

RemidyStatusCode remidy_catalog_load(RemidyPluginCatalog* catalog, const char* path) {
    try {
        std::filesystem::path fs_path(path);
        reinterpret_cast<PluginCatalog*>(catalog)->load(fs_path);
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_catalog_save(RemidyPluginCatalog* catalog, const char* path) {
    try {
        std::filesystem::path fs_path(path);
        reinterpret_cast<PluginCatalog*>(catalog)->save(fs_path);
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

int remidy_catalog_get_plugin_count(RemidyPluginCatalog* catalog) {
    auto plugins = reinterpret_cast<PluginCatalog*>(catalog)->getPlugins();
    return static_cast<int>(plugins.size());
}

RemidyPluginCatalogEntry* remidy_catalog_get_plugin_at(RemidyPluginCatalog* catalog, int index) {
    auto plugins = reinterpret_cast<PluginCatalog*>(catalog)->getPlugins();
    if (index < 0 || index >= static_cast<int>(plugins.size())) {
        return nullptr;
    }
    return reinterpret_cast<RemidyPluginCatalogEntry*>(plugins[index]);
}

// ========== PluginCatalogEntry API ==========

const char* remidy_entry_get_format(RemidyPluginCatalogEntry* entry) {
    return reinterpret_cast<PluginCatalogEntry*>(entry)->format().c_str();
}

const char* remidy_entry_get_plugin_id(RemidyPluginCatalogEntry* entry) {
    return reinterpret_cast<PluginCatalogEntry*>(entry)->pluginId().c_str();
}

const char* remidy_entry_get_display_name(RemidyPluginCatalogEntry* entry) {
    return reinterpret_cast<PluginCatalogEntry*>(entry)->displayName().c_str();
}

const char* remidy_entry_get_vendor_name(RemidyPluginCatalogEntry* entry) {
    return reinterpret_cast<PluginCatalogEntry*>(entry)->vendorName().c_str();
}

const char* remidy_entry_get_product_url(RemidyPluginCatalogEntry* entry) {
    return reinterpret_cast<PluginCatalogEntry*>(entry)->productUrl().c_str();
}

const char* remidy_entry_get_bundle_path(RemidyPluginCatalogEntry* entry) {
    static thread_local std::string path_str;
    path_str = reinterpret_cast<PluginCatalogEntry*>(entry)->bundlePath().string();
    return path_str.c_str();
}

// ========== PluginScanTool API ==========

RemidyPluginScanTool* remidy_scan_tool_create() {
    return reinterpret_cast<RemidyPluginScanTool*>(new PluginScanTool());
}

void remidy_scan_tool_destroy(RemidyPluginScanTool* tool) {
    delete reinterpret_cast<PluginScanTool*>(tool);
}

RemidyPluginCatalog* remidy_scan_tool_get_catalog(RemidyPluginScanTool* tool) {
    return reinterpret_cast<RemidyPluginCatalog*>(
        &reinterpret_cast<PluginScanTool*>(tool)->catalog
    );
}

RemidyStatusCode remidy_scan_tool_perform_scanning(RemidyPluginScanTool* tool) {
    try {
        int result = reinterpret_cast<PluginScanTool*>(tool)->performPluginScanning();
        return result == 0 ? REMIDY_OK : REMIDY_ERROR;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_scan_tool_save_cache(RemidyPluginScanTool* tool, const char* path) {
    try {
        std::filesystem::path fs_path(path);
        reinterpret_cast<PluginScanTool*>(tool)->savePluginListCache(fs_path);
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

void remidy_scan_tool_set_cache_file(RemidyPluginScanTool* tool, const char* path) {
    std::filesystem::path fs_path(path);
    reinterpret_cast<PluginScanTool*>(tool)->pluginListCacheFile() = fs_path;
}

// ========== PluginFormat API ==========

int remidy_scan_tool_get_format_count(RemidyPluginScanTool* tool) {
    auto formats = reinterpret_cast<PluginScanTool*>(tool)->formats();
    return static_cast<int>(formats.size());
}

RemidyPluginFormatInfo remidy_scan_tool_get_format_at(RemidyPluginScanTool* tool, int index) {
    static thread_local std::string format_name;
    auto formats = reinterpret_cast<PluginScanTool*>(tool)->formats();

    RemidyPluginFormatInfo info = {nullptr, nullptr};
    if (index >= 0 && index < static_cast<int>(formats.size())) {
        auto* format = formats[index];
        format_name = format->name();
        info.name = format_name.c_str();
        info.handle = reinterpret_cast<RemidyPluginFormat*>(format);
    }
    return info;
}

// ========== PluginInstance API ==========

// Helper struct to hold callback data
struct InstanceCreateCallbackData {
    RemidyInstanceCreateCallback callback;
    void* user_data;
};

void remidy_instance_create_async(
    RemidyPluginFormat* format,
    RemidyPluginCatalogEntry* entry,
    RemidyInstanceCreateCallback callback,
    void* user_data
) {
    try {
        auto* fmt = reinterpret_cast<PluginFormat*>(format);
        auto* ent = reinterpret_cast<PluginCatalogEntry*>(entry);

        // Create callback data on heap to survive this function scope
        auto* cb_data = new InstanceCreateCallbackData{callback, user_data};

        fmt->createInstance(ent, {}, [cb_data](std::unique_ptr<PluginInstance> instance, std::string error) {
            // Call the user's callback
            if (cb_data->callback) {
                RemidyPluginInstance* inst = instance ? reinterpret_cast<RemidyPluginInstance*>(instance.release()) : nullptr;
                const char* err = error.empty() ? nullptr : error.c_str();
                cb_data->callback(inst, err, cb_data->user_data);
            }
            delete cb_data;
        });
    } catch (const std::exception& e) {
        // Call callback with error
        if (callback) {
            callback(nullptr, e.what(), user_data);
        }
    } catch (...) {
        if (callback) {
            callback(nullptr, "Unknown error", user_data);
        }
    }
}

RemidyPluginInstance* remidy_instance_create(
    RemidyPluginFormat* format,
    RemidyPluginCatalogEntry* entry
) {
    try {
        auto* fmt = reinterpret_cast<PluginFormat*>(format);
        auto* ent = reinterpret_cast<PluginCatalogEntry*>(entry);

        // createInstance is now async with callback, so we need to block and wait
        PluginInstance* result = nullptr;
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;

        fmt->createInstance(ent, {}, [&](std::unique_ptr<PluginInstance> instance, std::string error) {
            std::lock_guard<std::mutex> lock(mtx);
            if (instance) {
                result = instance.release();
            }
            done = true;
            cv.notify_one();
        });

        // Wait for callback
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&]{ return done; });

        return reinterpret_cast<RemidyPluginInstance*>(result);
    } catch (...) {
        return nullptr;
    }
}

void remidy_instance_destroy(RemidyPluginInstance* instance) {
    std::unique_ptr<PluginInstance>(reinterpret_cast<PluginInstance*>(instance));
}

RemidyStatusCode remidy_instance_configure(
    RemidyPluginInstance* instance,
    RemidyConfigurationRequest* config
) {
    try {
        PluginInstance::ConfigurationRequest req;
        req.sampleRate = config->sample_rate;
        req.bufferSizeInSamples = config->buffer_size_in_samples;
        req.offlineMode = config->offline_mode;

        if (config->has_main_input_channels) {
            req.mainInputChannels = config->main_input_channels;
        }
        if (config->has_main_output_channels) {
            req.mainOutputChannels = config->main_output_channels;
        }

        auto status = reinterpret_cast<PluginInstance*>(instance)->configure(req);
        return to_c_status(status);
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_start_processing(RemidyPluginInstance* instance) {
    try {
        auto status = reinterpret_cast<PluginInstance*>(instance)->startProcessing();
        return to_c_status(status);
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_stop_processing(RemidyPluginInstance* instance) {
    try {
        auto status = reinterpret_cast<PluginInstance*>(instance)->stopProcessing();
        return to_c_status(status);
    } catch (...) {
        return REMIDY_ERROR;
    }
}

// ========== Parameter API ==========

int remidy_instance_get_parameter_count(RemidyPluginInstance* instance) {
    try {
        auto* inst = reinterpret_cast<PluginInstance*>(instance);
        auto* paramSupport = inst->parameters();
        if (!paramSupport) return 0;
        auto& params = paramSupport->parameters();
        return static_cast<int>(params.size());
    } catch (...) {
        return 0;
    }
}

RemidyStatusCode remidy_instance_get_parameter_info(
    RemidyPluginInstance* instance,
    int index,
    RemidyParameterInfo* info
) {
    try {
        auto* inst = reinterpret_cast<PluginInstance*>(instance);
        auto* paramSupport = inst->parameters();
        if (!paramSupport) return REMIDY_NOT_SUPPORTED;

        auto& params = paramSupport->parameters();
        if (index < 0 || static_cast<size_t>(index) >= params.size())
            return REMIDY_INVALID_PARAMETER;

        auto* param = params[index];
        if (!param) return REMIDY_INVALID_PARAMETER;

        info->id = param->index();
        strncpy(info->name, param->name().c_str(), sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        info->min_value = param->minPlainValue();
        info->max_value = param->maxPlainValue();
        info->default_value = param->defaultPlainValue();
        info->is_automatable = param->automatable();
        info->is_readonly = !param->readable();

        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_get_parameter_value(
    RemidyPluginInstance* instance,
    uint32_t param_id,
    double* value
) {
    try {
        auto* inst = reinterpret_cast<PluginInstance*>(instance);
        auto* paramSupport = inst->parameters();
        if (!paramSupport) return REMIDY_NOT_SUPPORTED;

        double val;
        auto status = paramSupport->getParameter(param_id, &val);
        if (status != StatusCode::OK) return to_c_status(status);

        *value = val;
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_set_parameter_value(
    RemidyPluginInstance* instance,
    uint32_t param_id,
    double value
) {
    try {
        auto* inst = reinterpret_cast<PluginInstance*>(instance);
        auto* paramSupport = inst->parameters();
        if (!paramSupport) return REMIDY_NOT_SUPPORTED;

        // timestamp of 0 means immediate
        auto status = paramSupport->setParameter(param_id, value, 0);
        return to_c_status(status);
    } catch (...) {
        return REMIDY_ERROR;
    }
}

// Helper to get UI support
static remidy::PluginUISupport* get_ui_support(RemidyPluginInstance* instance) {
    auto* inst = reinterpret_cast<PluginInstance*>(instance);
    if (!inst) return nullptr;
    return inst->ui();
}

bool remidy_instance_has_ui(RemidyPluginInstance* instance) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return false;
        return ui->hasUI();
    } catch (...) {
        return false;
    }
}

RemidyStatusCode remidy_instance_create_ui(
    RemidyPluginInstance* instance,
    bool is_floating,
    uintptr_t parent_handle,
    RemidyUIResizeCallback resize_callback,
    void* user_data
) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return REMIDY_NOT_SUPPORTED;

        std::function<bool(uint32_t, uint32_t)> resizeHandler;
        if (resize_callback) {
            resizeHandler = [resize_callback, user_data](uint32_t width, uint32_t height) {
                return resize_callback(width, height, user_data);
            };
        }

        void* parent = reinterpret_cast<void*>(parent_handle);
        if (!ui->create(is_floating, parent, resizeHandler)) {
            return REMIDY_ERROR;
        }
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

void remidy_instance_destroy_ui(RemidyPluginInstance* instance) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return;
        ui->destroy();
    } catch (...) {
    }
}

RemidyStatusCode remidy_instance_show_ui(RemidyPluginInstance* instance) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return REMIDY_NOT_SUPPORTED;
        std::fprintf(stderr, "[remidy-node] remidy_instance_show_ui calling ui->show()\n");
        if (!ui->show()) return REMIDY_ERROR;
        std::fprintf(stderr, "[remidy-node] remidy_instance_show_ui ui->show() succeeded\n");
        return REMIDY_OK;
    } catch (...) {
        std::fprintf(stderr, "[remidy-node] remidy_instance_show_ui threw\n");
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_hide_ui(RemidyPluginInstance* instance) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return REMIDY_NOT_SUPPORTED;
        ui->hide();
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_get_ui_size(
    RemidyPluginInstance* instance,
    uint32_t* width,
    uint32_t* height
) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return REMIDY_NOT_SUPPORTED;
        if (!width || !height) return REMIDY_INVALID_PARAMETER;
        uint32_t w = 0, h = 0;
        if (!ui->getSize(w, h)) return REMIDY_ERROR;
        *width = w;
        *height = h;
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

RemidyStatusCode remidy_instance_set_ui_size(
    RemidyPluginInstance* instance,
    uint32_t width,
    uint32_t height
) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return REMIDY_NOT_SUPPORTED;
        if (!ui->setSize(width, height)) return REMIDY_ERROR;
        return REMIDY_OK;
    } catch (...) {
        return REMIDY_ERROR;
    }
}

bool remidy_instance_can_ui_resize(RemidyPluginInstance* instance) {
    try {
        auto* ui = get_ui_support(instance);
        if (!ui) return false;
        return ui->canResize();
    } catch (...) {
        return false;
    }
}

RemidyContainerWindow* remidy_container_window_create(
    const char* title,
    int width,
    int height,
    RemidyContainerWindowCloseCallback callback,
    void* user_data
) {
    try {
        auto* wrapper = new RemidyContainerWindow();
        wrapper->closeCallback = callback;
        wrapper->closeContext = user_data;
        auto created = runOnMainThreadSync([&]() -> bool {
            remidy::gui::GLContextGuard guard;
            wrapper->window = remidy::gui::ContainerWindow::create(title, width, height, [wrapper]() {
                if (wrapper->closeCallback) {
                    wrapper->closeCallback(wrapper->closeContext);
                }
            });
            return wrapper->window != nullptr;
        });
        if (!created) {
            delete wrapper;
            return nullptr;
        }
        return wrapper;
    } catch (...) {
        return nullptr;
    }
}

void remidy_container_window_destroy(RemidyContainerWindow* window) {
    if (!window) return;
    runOnMainThreadSync([&]() {
        remidy::gui::GLContextGuard guard;
        window->window.reset();
    });
    delete window;
}

void remidy_container_window_show(RemidyContainerWindow* window, bool visible) {
    if (!window || !window->window) return;
    runOnMainThreadSync([&]() {
        remidy::gui::GLContextGuard guard;
        window->window->show(visible);
    });
}

void remidy_container_window_resize(RemidyContainerWindow* window, int width, int height) {
    if (!window || !window->window) return;
    runOnMainThreadSync([&]() {
        remidy::gui::GLContextGuard guard;
        window->window->resize(width, height);
    });
}

RemidyBounds remidy_container_window_get_bounds(RemidyContainerWindow* window) {
    RemidyBounds bounds{0, 0, 0, 0};
    if (!window || !window->window) return bounds;
    auto cpp_bounds = runOnMainThreadSync([&]() {
        return window->window->getBounds();
    });
    bounds.x = cpp_bounds.x;
    bounds.y = cpp_bounds.y;
    bounds.width = cpp_bounds.width;
    bounds.height = cpp_bounds.height;
    return bounds;
}

uintptr_t remidy_container_window_get_handle(RemidyContainerWindow* window) {
    if (!window || !window->window) return 0;
    return runOnMainThreadSync([&]() -> uintptr_t {
        return reinterpret_cast<uintptr_t>(window->window->getHandle());
    });
}

RemidyGLContextGuard* remidy_gl_context_guard_create() {
    try {
        auto* wrapper = new RemidyGLContextGuard();
        wrapper->guard = std::make_unique<remidy::gui::GLContextGuard>();
        return wrapper;
    } catch (...) {
        return nullptr;
    }
}

void remidy_gl_context_guard_destroy(RemidyGLContextGuard* guard) {
    if (!guard) return;
    delete guard;
}

// ========== EventLoop API ==========

void remidy_eventloop_init_nodejs(
    void (*enqueue_callback)(RemidyMainThreadTask task, void* user_data, void* context),
    void* context
) {
    if (!nodejs_event_loop) {
        nodejs_event_loop = new NodeJSEventLoop(enqueue_callback, context);
        remidy::setEventLoop(nodejs_event_loop);
    }
}

} // extern "C"
