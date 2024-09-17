#pragma once

#include <string>
#include <filesystem>
#include "Common.hpp"
#include "AudioPluginFormat.hpp"

namespace remidy {

    class AudioPluginLibrary {
        std::filesystem::path& libraryFile;
        std::vector<AudioPluginIdentifier> ids;
    public:
        AudioPluginLibrary(std::filesystem::path& libraryFile, std::vector<AudioPluginIdentifier> ids)
            : libraryFile(libraryFile), ids(std::move(ids)) {}
        std::filesystem::path& getLibraryFile() { return libraryFile; }
        std::vector<AudioPluginIdentifier>& getPluginIds() { return ids; }
    };
}
