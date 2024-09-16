#pragma once

#include <cstdint>
#include <string>

typedef int32_t remidy_status_t;

namespace remidy {
    class AudioPluginLibrary;

    class AudioPluginIdentifier {
    public:
        AudioPluginIdentifier() = default;
        virtual ~AudioPluginIdentifier() = default;
        virtual std::string& getUniqueId() = 0;
        virtual std::string& getDisplayName() = 0;
    };
}
