#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <filesystem>
#include <vector>
#include <functional>


typedef int32_t remidy_status_t;
typedef uint32_t remidy_ump_t;
typedef int64_t remidy_timestamp_t;

namespace remidy {

    class AudioBufferList {
    public:
        float* getFloatBufferForChannel(int32_t channel);
        double* getDoubleBufferForChannel(int32_t channel);
        int32_t size();
    };

    // Represents a sample-accurate sequence of UMPs
    class MidiSequence {
        std::vector<remidy_ump_t> messages;
    public:
        remidy_ump_t* getMessages();
        size_t sizeInInts();
        size_t sizeInBytes();
    };



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
        remidy_status_t pluginId(std::string& newId) {
            id = newId;
            return remidy_status_t(0); // FIXME: define constants
        }
        // Returns a file system path to the bundle, if the format supports it.
        std::filesystem::path& bundlePath() { return bundle; }
        // Sets a file system path to the bundle, if the format supports it.
        remidy_status_t bundlePath(const std::filesystem::path& newPath) {
            bundle = newPath;
            return remidy_status_t(0); // FIXME: define constants
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
        std::vector<std::unique_ptr<PluginCatalogEntry>> list;

    public:
        std::vector<std::unique_ptr<PluginCatalogEntry>>& getPlugins() { return list; }
        void load(std::filesystem::path path);
        void save(std::filesystem::path path);
    };


    class AudioPluginLibraryPool {
    public:
        explicit AudioPluginLibraryPool(
            std::function<remidy_status_t(std::filesystem::path& moduleBundlePath, void** module)>& load,
            std::function<remidy_status_t(std::filesystem::path& moduleBundlePath, void* module)>& unload
        );
        virtual ~AudioPluginLibraryPool();

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
        remidy_status_t removeReference(std::filesystem::path& moduleBundlePath);

    private:
        std::function<remidy_status_t(std::filesystem::path& moduleBundlePath, void** module)> load;
        std::function<remidy_status_t(std::filesystem::path& moduleBundlePath, void* module)> unload;
        RetentionPolicy retentionPolicy{UnloadImmediately};
        std::map<std::filesystem::path, ModuleEntry> entries{};
    };


    class AudioProcessContext {
    public:
        AudioProcessContext();

        AudioBufferList input{};
        AudioBufferList output{};
        MidiSequence midi{};
    };

    class AudioPluginInstance {
    protected:
        explicit AudioPluginInstance() = default;

    public:
        virtual ~AudioPluginInstance() = default;

        class Extensibility {
            AudioPluginInstance& owner;
        protected:
            explicit Extensibility(AudioPluginInstance& owner) : owner(owner) {
            }
            virtual ~Extensibility() = default;
        };

        virtual Extensibility* getExtensibility() { return nullptr; }

        virtual remidy_status_t configure(int32_t sampleRate) = 0;

        virtual remidy_status_t process(AudioProcessContext& process) = 0;
    };

    class AudioPluginFormat {
        class Impl;
        Impl *impl{};

    protected:
        AudioPluginFormat();
        virtual ~AudioPluginFormat() = default;

    public:
        enum ScanningStrategyValue {
            NO,
            MAYBE,
            YES
        };

        bool hasPluginListCache();
        bool supportsGlobalIdentifier();
        // Indicates whether the plugin API requires sample rate at *instantiating*.
        // Only LV2 requires this, among VST3, AUv2/v3, LV2, and CLAP.
        // This impacts on whether we will have to discard and instantiate a plugin
        // when our use app changes the sample rate.
        bool instantiateRequiresSampleRate();
        std::vector<PluginCatalogEntry*> getAvailablePlugins();

        virtual bool usePluginSearchPaths() = 0;
        virtual std::vector<std::string>& getDefaultSearchPaths() = 0;
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        virtual std::vector<PluginCatalogEntry*> scanAllAvailablePlugins() = 0;

        virtual std::string savePluginInformation(PluginCatalogEntry* identifier) = 0;
        virtual std::string savePluginInformation(AudioPluginInstance* instance) = 0;
        virtual std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string& data) = 0;

        virtual AudioPluginInstance* createInstance(PluginCatalogEntry* uniqueId) = 0;
    };

    class DesktopAudioPluginFormat : public AudioPluginFormat {
    protected:
        explicit DesktopAudioPluginFormat() = default;

        std::vector<std::string> overrideSearchPaths{};

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }

        virtual std::vector<std::unique_ptr<PluginCatalogEntry>> createPluginInformation(std::filesystem::path &bundlePath) = 0;
    };


    class AudioPluginFormatVST3 : public DesktopAudioPluginFormat {
    public:
        class Impl;

        explicit AudioPluginFormatVST3(std::vector<std::string>& overrideSearchPaths);
        ~AudioPluginFormatVST3() override;

        bool usePluginSearchPaths() override;
        std::vector<std::string>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<PluginCatalogEntry*> scanAllAvailablePlugins() override;

        std::string savePluginInformation(PluginCatalogEntry* identifier) override;
        std::string savePluginInformation(AudioPluginInstance* instance) override;
        std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string& data) override;

        AudioPluginInstance* createInstance(PluginCatalogEntry* uniqueId) override;

        std::vector<std::unique_ptr<PluginCatalogEntry>> createPluginInformation(std::filesystem::path &bundlePath) override;

    private:
        Impl *impl;
    };

}
