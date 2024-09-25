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
            std::function<remidy_status_t(std::filesystem::path& libraryFile, void** module)>& load,
            std::function<remidy_status_t(std::filesystem::path& libraryFile, void* module)>& unload
        );
        virtual ~AudioPluginLibraryPool();

        struct ModuleEntry {
            uint32_t refCount;
            std::filesystem::path vst3Dir;
            void* module;
        };
        enum RetentionPolicy {
            Retain,
            UnloadImmediately,
            UnloadFromLRU
        };

        RetentionPolicy getRetentionPolicy();
        void setRetentionPolicy(RetentionPolicy value);
        void* loadOrAddReference(std::filesystem::path& libraryFile);
        remidy_status_t removeReference(std::filesystem::path& libraryFile);

    private:
        std::function<remidy_status_t(std::filesystem::path& vst3Dir, void** module)> load;
        std::function<remidy_status_t(std::filesystem::path& vst3Dir, void* module)> unload;
        // FIXME: the default should be `UnloadImmediately`.
        RetentionPolicy retentionPolicy{Retain};
        std::map<std::filesystem::path, ModuleEntry> entries{};
    };
}
