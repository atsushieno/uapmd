#pragma once
#include <map>
#include <filesystem>
#include <vector>

#include "../remidy.hpp"

namespace remidy {

    class PluginCatalogEntry {
    public:
        enum MetadataPropertyID {
            DisplayName,
            VendorName,
            ProductUrl
        };

    private:
        std::string id;
        std::filesystem::path bundle;
        std::map<MetadataPropertyID, std::string> props{};

    public:
        PluginCatalogEntry() = default;
        virtual ~PluginCatalogEntry() = default;

        // Returns the plugin ID.
        std::string pluginId() { return id; }
        // Set a new plugin ID. It is settable only because deserializers will use it.
        StatusCode pluginId(std::string& newId) {
            id = newId;
            return StatusCode::OK;
        }
        // Returns a file system path to the bundle, if the format supports it.
        std::filesystem::path& bundlePath() { return bundle; }
        // Sets a file system path to the bundle, if the format supports it.
        StatusCode bundlePath(const std::filesystem::path& newPath) {
            bundle = newPath;
            return StatusCode::OK;
        }
        std::string getMetadataProperty(const MetadataPropertyID id) {
            const auto ret = props.find(id);
            return ret == props.end() ? std::string{} : ret->second;
        }
        void setMetadataProperty(const MetadataPropertyID id, const std::string& value) {
            props[id] = value;
        }
    };

    class PluginCatalog {
        std::vector<std::unique_ptr<PluginCatalogEntry>> list{};

    public:
        std::vector<PluginCatalogEntry*> getPlugins();
        void add(std::unique_ptr<PluginCatalogEntry> entry);
        void merge(PluginCatalog&& other);
        void clear();
        void load(std::filesystem::path path);
        void save(std::filesystem::path path);
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
        void* loadOrAddReference(std::filesystem::path& moduleBundlePath);
        StatusCode removeReference(std::filesystem::path& moduleBundlePath);

    private:
        std::function<StatusCode(std::filesystem::path& moduleBundlePath, void** module)> load;
        std::function<StatusCode(std::filesystem::path& moduleBundlePath, void* module)> unload;
        RetentionPolicy retentionPolicy{UnloadImmediately};
        std::map<std::filesystem::path, ModuleEntry> entries{};
    };

}