#pragma once

#include <string>
#include "Common.hpp"
#include "AudioPluginFormat.hpp"

namespace remidy {

    class AudioPluginLibrary {
        std::string directory;
        std::vector<AudioPluginIdentifier> ids;
    public:
        AudioPluginLibrary(std::string directory, std::vector<AudioPluginIdentifier> ids)
            : directory(std::move(directory)), ids(std::move(ids)) {}
        std::string& getDirectory() { return directory; }
        std::vector<AudioPluginIdentifier>& getPluginIds() { return ids; }
    };
}
