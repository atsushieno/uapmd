#pragma once

#include <memory>
#include <string>
#include <vector>
#include "uapmd/priv/CommonTypes.hpp"
#include "AudioPluginHostPAL.hpp"
#include "uapmd/priv/midi/UapmdUmpMapper.hpp"

namespace uapmd {

    struct ParameterNamedValue {
        double value;
        std::string name;
    };

    struct ParameterMetadata {
        uint32_t index;
        std::string stableId;
        std::string name;
        std::string path;
        double defaultPlainValue;
        double minPlainValue;
        double maxPlainValue;
        bool automatable;
        bool hidden;
        bool discrete;
        std::vector<ParameterNamedValue> namedValues{};
    };

    struct PresetsMetadata {
        uint8_t bank;
        uint32_t index;
        std::string stableId;
        std::string name;
        std::string path;
    };

    class AudioPluginNode {
        std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> node_;
        bool bypassed_{false};
        int32_t instance_id_;
        std::unique_ptr<UapmdUmpInputMapper> ump_input_mapper{};

    public:
        AudioPluginNode(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL, int32_t instanceId);
        virtual ~AudioPluginNode();

        AudioPluginHostPAL::AudioPluginNodePAL* pal();

        // instanceId can be used to indicate a plugin instance *across* processes i.e.
        // where pointer to the instance cannot be shared.
        int32_t instanceId();

        bool bypassed();
        void bypassed(bool value);

        uapmd_status_t processAudio(AudioProcessContext& process);

        void loadState(std::vector<uint8_t>& state);
        std::vector<uint8_t> saveState();
    };

}
