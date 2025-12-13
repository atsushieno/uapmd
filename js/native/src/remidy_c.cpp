#include "../include/remidy_c.h"
#include "remidy/remidy.hpp"
#include "remidy-tooling/PluginScanTool.hpp"
#include <cstring>
#include <memory>

using namespace remidy;
using namespace remidy_tooling;

// Helper to convert C++ StatusCode to C enum
static RemidyStatusCode to_c_status(StatusCode status) {
    switch (status) {
        case StatusCode::OK: return REMIDY_OK;
        case StatusCode::Error: return REMIDY_ERROR;
        case StatusCode::NotSupported: return REMIDY_NOT_SUPPORTED;
        case StatusCode::InvalidParameter: return REMIDY_INVALID_PARAMETER;
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

RemidyPluginInstance* remidy_instance_create(
    RemidyPluginFormat* format,
    RemidyPluginCatalogEntry* entry
) {
    try {
        auto* fmt = reinterpret_cast<PluginFormat*>(format);
        auto* ent = reinterpret_cast<PluginCatalogEntry*>(entry);
        auto instance = fmt->createInstance(ent);
        return reinterpret_cast<RemidyPluginInstance*>(instance.release());
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
        auto* params = reinterpret_cast<PluginInstance*>(instance)->parameters();
        if (!params) return 0;
        return static_cast<int>(params->parameterCount());
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
        auto* params = reinterpret_cast<PluginInstance*>(instance)->parameters();
        if (!params) return REMIDY_NOT_SUPPORTED;

        auto meta = params->parameterMetadata(index);
        if (!meta) return REMIDY_INVALID_PARAMETER;

        info->id = meta->id;
        strncpy(info->name, meta->displayName.c_str(), sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        info->min_value = meta->minValue;
        info->max_value = meta->maxValue;
        info->default_value = meta->defaultValue;
        info->is_automatable = meta->isAutomatable;
        info->is_readonly = meta->isReadOnly;

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
        auto* params = reinterpret_cast<PluginInstance*>(instance)->parameters();
        if (!params) return REMIDY_NOT_SUPPORTED;

        auto val = params->getParameter(param_id);
        if (!val.has_value()) return REMIDY_INVALID_PARAMETER;

        *value = val.value();
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
        auto* params = reinterpret_cast<PluginInstance*>(instance)->parameters();
        if (!params) return REMIDY_NOT_SUPPORTED;

        auto status = params->setParameter(param_id, value);
        return to_c_status(status);
    } catch (...) {
        return REMIDY_ERROR;
    }
}

} // extern "C"
