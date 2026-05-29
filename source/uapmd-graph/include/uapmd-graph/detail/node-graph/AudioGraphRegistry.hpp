#pragma once

#include <memory>
#include <string_view>

#include "AudioGraphBuiltInNodeFactory.hpp"

namespace uapmd {

    class AudioGraphRegistry {
    public:
        virtual ~AudioGraphRegistry() = default;

        virtual void registerBuiltInFactory(std::unique_ptr<AudioGraphBuiltInNodeFactory> factory) = 0;
        virtual const AudioGraphBuiltInNodeFactory* findBuiltInFactory(std::string_view nodeType) const = 0;

        static std::unique_ptr<AudioGraphRegistry> createDefault();
    };

}
