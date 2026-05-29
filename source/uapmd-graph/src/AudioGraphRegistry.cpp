#include "uapmd-graph/detail/node-graph/AudioGraphRegistry.hpp"

#include <string>
#include <unordered_map>

namespace uapmd {

    namespace {

        class AudioGraphRegistryImpl : public AudioGraphRegistry {
            std::unordered_map<std::string, std::unique_ptr<AudioGraphBuiltInNodeFactory>> factories_{};

        public:
            void registerBuiltInFactory(std::unique_ptr<AudioGraphBuiltInNodeFactory> factory) override {
                if (!factory)
                    return;
                factories_[std::string(factory->nodeType())] = std::move(factory);
            }

            const AudioGraphBuiltInNodeFactory* findBuiltInFactory(std::string_view nodeType) const override {
                auto it = factories_.find(std::string(nodeType));
                return it != factories_.end() ? it->second.get() : nullptr;
            }
        };

    }

    std::unique_ptr<AudioGraphRegistry> AudioGraphRegistry::createDefault() {
        return std::make_unique<AudioGraphRegistryImpl>();
    }

}
