#pragma once

#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <typeinfo>

#include "uapmd/uapmd.hpp"
#include "AudioGraphExtension.hpp"
#include "AudioGraphNode.hpp"

namespace uapmd {

    class AudioGraph {
    protected:
        explicit AudioGraph(std::string providerId = {})
            : provider_id_(std::move(providerId)) {}

        virtual AudioGraphExtension* getExtension(const std::type_info& type) = 0;
        virtual const AudioGraphExtension* getExtension(const std::type_info& type) const = 0;

    public:
        virtual ~AudioGraph() = default;

        const std::string& providerId() const {
            return provider_id_;
        }

        virtual std::map<std::string, AudioGraphNode*> nodes() = 0;
        virtual AudioGraphNode* getNode(const std::string& nodeId) = 0;

        virtual void setGroupResolver(std::function<uint8_t(int32_t)> resolver) = 0;
        virtual void setEventOutputCallback(std::function<void(int32_t instanceId, const uapmd_ump_t* data, size_t dataSizeInBytes)> callback) = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;
        virtual uint32_t outputBusCount() = 0;
        virtual uint32_t outputLatencyInSamples(uint32_t outputBusIndex) = 0;
        virtual double outputTailLengthInSeconds(uint32_t outputBusIndex) = 0;
        virtual uint32_t renderLeadInSamples() = 0;
        virtual uint32_t mainOutputLatencyInSamples() = 0;
        virtual double mainOutputTailLengthInSeconds() = 0;

        template <typename T>
        T* getExtension() {
            static_assert(std::is_base_of_v<AudioGraphExtension, T>);
            return dynamic_cast<T*>(getExtension(typeid(T)));
        }

        template <typename T>
        const T* getExtension() const {
            static_assert(std::is_base_of_v<AudioGraphExtension, T>);
            return dynamic_cast<const T*>(getExtension(typeid(T)));
        }

    private:
        std::string provider_id_{};
    };

}
