#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <filesystem>
#include <vector>
#include <functional>


typedef uint32_t remidy_ump_t;
typedef int64_t remidy_timestamp_t;

namespace remidy {

    enum StatusCode {
        OK,
        BUNDLE_NOT_FOUND,
        FAILED_TO_INSTANTIATE
    };

    class Logger {
    public:
        class Impl;

        enum LogLevel {
            DIAGNOSTIC,
            INFO,
            WARNING,
            ERROR
        };

        static Logger* global();
        void logError(const char* format, ...);
        void logWarning(const char* format, ...);
        void logInfo(const char* format, ...);
        void logDiagnostic(const char* format, ...);
        static void stopDefaultLogger();

        Logger();
        ~Logger();

        void log(LogLevel level, const char* format, ...);
        void logv(LogLevel level, const char* format, va_list args);

        std::vector<std::function<void(LogLevel level, size_t serial, const char* s)>> callbacks;

    private:
        Impl *impl{nullptr};
    };

    // Represents a list of audio buffers, separate per channel.
    // It is part of `AudioProcessingContext`.
    class AudioBufferList {
    public:
        float* getFloatBufferForChannel(int32_t channel);
        double* getDoubleBufferForChannel(int32_t channel);
        int32_t size();
    };

    // Represents a sample-accurate sequence of UMPs.
    // It is part of `AudioProcessingContext`.
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


    // Facade to extension points in audio plugin abstraction layers such as
    // `AudioPluginFormat` and `AudioPluginInstance`.
    // Each extendable class implementors provide a derived class and provide
    // a getter that users of the extendable class can downcast to each class.
    // See how `AudioPluginFormatVST3::getExtensibility()` works for example.
    template <typename T>
    class AudioPluginExtensibility {
        T& owner;
    protected:
        explicit AudioPluginExtensibility(T& owner) : owner(owner) {
        }
        virtual ~AudioPluginExtensibility() = default;
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

        virtual AudioPluginExtensibility<AudioPluginInstance>* getExtensibility() { return nullptr; }

        virtual StatusCode configure(int32_t sampleRate) = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;
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

        virtual AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() { return nullptr; }

        bool hasPluginListCache();
        bool supportsGlobalIdentifier();
        // Indicates whether the plugin API requires sample rate at *instantiating*.
        // Only LV2 requires this, among VST3, AUv2/v3, LV2, and CLAP.
        // This impacts on whether we will have to discard and instantiate a plugin
        // when our use app changes the sample rate.
        bool instantiateRequiresSampleRate();
        PluginCatalog& getAvailablePlugins();

        virtual bool usePluginSearchPaths() = 0;
        virtual std::vector<std::string>& getDefaultSearchPaths() = 0;
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        virtual PluginCatalog scanAllAvailablePlugins() = 0;

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

        virtual PluginCatalog createCatalogFragment(std::filesystem::path &bundlePath) = 0;
    };


    class AudioPluginFormatVST3 : public DesktopAudioPluginFormat {
    public:
        class Impl;

        class Extensibility : public AudioPluginExtensibility<AudioPluginFormat> {
            bool report_not_implemented{false};
        public:
            explicit Extensibility(AudioPluginFormat& format);

            bool reportNotImplemented() { return report_not_implemented; }
            StatusCode reportNotImplemented(bool newValue) {
                report_not_implemented = newValue;
                return StatusCode::OK;
            }
        };

        explicit AudioPluginFormatVST3(std::vector<std::string>& overrideSearchPaths);
        ~AudioPluginFormatVST3() override;

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        bool usePluginSearchPaths() override;
        std::vector<std::string>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        PluginCatalog scanAllAvailablePlugins() override;

        std::string savePluginInformation(PluginCatalogEntry* identifier) override;
        std::string savePluginInformation(AudioPluginInstance* instance) override;
        std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string& data) override;

        AudioPluginInstance* createInstance(PluginCatalogEntry* uniqueId) override;

        PluginCatalog createCatalogFragment(std::filesystem::path &bundlePath) override;

    private:
        Impl *impl;
    };

}
