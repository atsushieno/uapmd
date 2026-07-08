#pragma once

#include "AudioGraphNode.hpp"

namespace uapmd {

    class AudioPluginNode : public AudioGraphNode, public AudioPluginNodeFeature {
    public:
        ~AudioPluginNode() override = default;

        virtual int32_t instanceId() const = 0;
        virtual AudioPluginInstanceAPI* instance() override = 0;
        virtual bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) override = 0;
        virtual void sendAllNotesOff() = 0;
        virtual void requestStopFlush() = 0;
        // Discard all queued and pending input events. Must only be called while
        // audio processing is not running (e.g. after the audio callback stopped).
        virtual void clearQueuedEvents() = 0;
        virtual ParameterUpdateEvent& parameterUpdateEvent() override = 0;
        virtual ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() override = 0;
    };

}
