#pragma once
#include <map>
#include <filesystem>
#include <vector>

#include "../remidy.hpp"

namespace remidy {

    class PluginCatalogEntry {
        std::string fmt{};
        std::string id{};
        std::string display_name{};
        std::string vendor_name{};
        std::string product_url{};
        std::filesystem::path bundle{};

    public:
        PluginCatalogEntry() = default;
        virtual ~PluginCatalogEntry() = default;

        // Returns the plugin format.
        std::string& format() { return fmt; }
        // Set a new plugin ID. It is settable only because deserializers will use it.
        StatusCode format(std::string& newFormat) {
            fmt = newFormat;
            return StatusCode::OK;
        }
        // Returns the plugin ID.
        std::string& pluginId() { return id; }
        // Set a new plugin ID. It is settable only because deserializers will use it.
        StatusCode pluginId(std::string& newId) {
            id = newId;
            return StatusCode::OK;
        }
        std::string& displayName() { return display_name; }
        StatusCode displayName(const std::string& newValue) {
            display_name = newValue;
            return StatusCode::OK;
        }
        std::string& vendorName() { return vendor_name; }
        StatusCode vendorName(const std::string& newValue) {
            vendor_name = newValue;
            return StatusCode::OK;
        }
        std::string& productUrl() { return product_url; }
        StatusCode productUrl(const std::string& newValue) {
            product_url = newValue;
            return StatusCode::OK;
        }
        // Returns a file system path to the bundle, if the format supports it.
        std::filesystem::path& bundlePath() { return bundle; }
        // Sets a file system path to the bundle, if the format supports it.
        StatusCode bundlePath(const std::filesystem::path& newPath) {
            bundle = newPath.lexically_normal();
            return StatusCode::OK;
        }
    };

    class PluginCatalog {
        std::vector<std::unique_ptr<PluginCatalogEntry>> entries{};
        std::vector<std::unique_ptr<PluginCatalogEntry>> denyList{};

    public:
        std::vector<PluginCatalogEntry*> getPlugins();
        std::vector<PluginCatalogEntry*> getDenyList();
        bool contains(std::string& format, std::string& pluginId) const;
        void add(std::unique_ptr<PluginCatalogEntry> entry);
        void merge(PluginCatalog&& other);
        void clear();
        void load(std::filesystem::path& path);
        void save(std::filesystem::path& path);
    };


    class PluginBundlePool {
    public:
        explicit PluginBundlePool(
            std::function<StatusCode(std::filesystem::path& moduleBundlePath, void** module)>& load,
            std::function<StatusCode(std::filesystem::path& moduleBundlePath, void* module)>& unload
        );
        virtual ~PluginBundlePool();

        struct ModuleEntry {
            uint32_t refCount;
            std::filesystem::path moduleBundlePath;
            void* module;
        };
        enum RetentionPolicy {
            Retain,
            UnloadImmediately,
        };

        RetentionPolicy getRetentionPolicy();
        void setRetentionPolicy(RetentionPolicy value);
        // Returns either HMODULE, CFBundle*, or dlopen-ed library.
        void* loadOrAddReference(std::filesystem::path& moduleBundlePath, bool* loadedAsNew);
        StatusCode removeReference(std::filesystem::path& moduleBundlePath);

    private:
        std::function<StatusCode(std::filesystem::path& moduleBundlePath, void** module)> load;
        std::function<StatusCode(std::filesystem::path& moduleBundlePath, void* module)> unload;
        RetentionPolicy retentionPolicy{UnloadImmediately};
        std::map<std::filesystem::path, ModuleEntry> entries{};
    };

}