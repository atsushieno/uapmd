#pragma once

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

    class PluginParameterFeature {
    public:
        virtual ~PluginParameterFeature() = default;

        virtual remidy::EventListenerId addParameterChangeListener(std::function<void(uint32_t paramIndex, double plainValue)> listener) = 0;
        virtual void removeParameterChangeListener(remidy::EventListenerId listenerId) = 0;
        virtual remidy::EventListenerId addParameterMetadataChangeListener(std::function<void()> listener) = 0;
        virtual void removeParameterMetadataChangeListener(remidy::EventListenerId listenerId) = 0;
        virtual remidy::EventListenerId addPerNoteControllerChangeListener(std::function<void(remidy::PerNoteControllerContextTypes types, uint32_t context, uint32_t parameterIndex, double value)>) = 0;
        virtual void removePerNoteControllerChangeListener(remidy::EventListenerId listenerId) = 0;

        virtual double normalizeParameterValue(int32_t parameterIndex, double plainValue) = 0;
        virtual double normalizePerNoteControllerValue(int32_t parameterIndex, remidy::PerNoteControllerContextTypes types, remidy::PerNoteControllerContext context, double plainValue) = 0;
    };

    class AudioPluginInstanceFeature {
    public:
        virtual ~AudioPluginInstanceFeature() = default;

        virtual uapmd_status_t processAudio(AudioProcessContext &process) = 0;

        virtual bool bypassed() const = 0;
        virtual void bypassed(bool value) = 0;

        virtual PluginParameterFeature* parameterSupport() = 0;
        virtual std::vector<ParameterMetadata> parameterMetadataList() = 0;
        virtual std::vector<ParameterMetadata> perNoteControllerMetadataList(remidy::PerNoteControllerContextTypes contextType, uint32_t context) = 0;
        virtual std::vector<PresetsMetadata> presetMetadataList() = 0;

        virtual double getParameterValue(int32_t index) = 0;
        virtual void setParameterValue(int32_t index, double value) = 0;
        virtual double getPerNoteControllerValue(remidy::PerNoteControllerContext context, uint8_t index) = 0;
        virtual void setPerNoteControllerValue(remidy::PerNoteControllerContext context, uint8_t index, double value) = 0;

        virtual void loadPreset(int32_t presetIndex) = 0;

        virtual std::vector<uint8_t> saveState() = 0;
        virtual void loadState(std::vector<uint8_t>& state) = 0;
    };
}