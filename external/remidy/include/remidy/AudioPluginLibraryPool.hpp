#pragma once

#include <string>
#include <filesystem>
#include <functional>
#include <map>

#include "Common.hpp"
#include "AudioPluginFormat.hpp"

namespace remidy {

	/*
    class AudioPluginLibrary {
        std::filesystem::path& libraryFile;
        std::vector<AudioPluginIdentifier> ids;
    public:
        AudioPluginLibrary(std::filesystem::path& libraryFile, std::vector<AudioPluginIdentifier> ids)
            : libraryFile(libraryFile) {}
        std::filesystem::path& getLibraryFile() { return libraryFile; }
        std::vector<AudioPluginIdentifier>& getPluginIds() { return ids; }
    };*/

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
            UnloadFromLRU
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
}
