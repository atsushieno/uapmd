#pragma once

#include <cstdint>
#include <string>

typedef int32_t remidy_status_t;
typedef uint32_t remidy_ump_t;
typedef int64_t remidy_timestamp_t;

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
