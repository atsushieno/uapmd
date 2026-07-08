#pragma once
#include <umppi/umppi.hpp>
#include "concurrentqueue.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <vector>
#include "uapmd/uapmd.hpp"
#include "uapmd-graph/detail/node-graph/AudioPluginNode.hpp"
#include "midi/UapmdNodeUmpMapper.hpp"

namespace uapmd {
#ifdef __EMSCRIPTEN__
    bool trySendWebClapInputEvents(AudioPluginInstanceAPI* instance, const uapmd_ump_t* events, size_t sizeInBytes);
#endif

    class AudioPluginNodeImpl : public AudioPluginNode {
        int32_t instance_id_;
        std::string node_id_;
        std::string node_type_;
        AudioPluginInstanceAPI* instance_;
        moodycamel::ConcurrentQueue<umppi::Ump> queue_;
        std::vector<umppi::Ump> pending_events_;
        std::function<void()> on_delete_;
        ParameterUpdateEvent parameter_update_event_;
        ParameterMetadataRefreshEvent parameter_metadata_refresh_event_;
        remidy::EventListenerId parameter_listener_token_{0};
        remidy::EventListenerId metadata_listener_token_{0};
        std::atomic<bool> stop_flush_requested_{false};
        std::unique_ptr<UapmdNodeUmpInputMapper> ump_input_mapper_{};
        // Active-note bitmask, indexed [group][channel][note / 64]. RT-thread only.
        // Used by prepareStopFlush() to emit genuine note-offs: formats like VST3 have
        // no All Sound Off event, so CC 120 alone cannot silence held voices.
        uint64_t active_notes_[16][16][2]{};
        bool has_active_notes_{false};

    public:
        AudioPluginNodeImpl(
            int32_t instanceId,
            AudioPluginInstanceAPI* instance,
            size_t eventBufferSizeInBytes,
            std::function<void()>&& onDelete
        ) : instance_id_(instanceId),
            node_id_("plugin:" + std::to_string(instanceId)),
            node_type_(instance && !instance->formatName().empty() ? "plugin:" + instance->formatName() : "plugin"),
            instance_(instance),
            queue_(eventBufferSizeInBytes),
            on_delete_(std::move(onDelete)) {
            pending_events_.reserve(eventBufferSizeInBytes / sizeof(umppi::Ump));
            if (instance_)
                ump_input_mapper_ = std::make_unique<UapmdNodeUmpInputMapper>(instance_);
            // Register parameter change listeners directly with the plugin
            if (instance_ && instance_->parameterSupport()) {
                parameter_listener_token_ = instance_->parameterSupport()->parameterChangeEvent().addListener(
                    [this](uint32_t paramIndex, double plainValue) {
                        parameter_update_event_.notify(static_cast<int32_t>(paramIndex), plainValue);
                    }
                );
                metadata_listener_token_ = instance_->parameterSupport()->parameterMetadataChangeEvent().addListener(
                    [this]() {
                        parameter_metadata_refresh_event_.notify();
                    }
                );
            }
        }

        ~AudioPluginNodeImpl() override {
            // Unregister parameter listeners
            if (instance_ && instance_->parameterSupport()) {
                if (parameter_listener_token_ != 0)
                    instance_->parameterSupport()->parameterChangeEvent().removeListener(parameter_listener_token_);
                if (metadata_listener_token_ != 0)
                    instance_->parameterSupport()->parameterMetadataChangeEvent().removeListener(metadata_listener_token_);
            }
            if (on_delete_)
                on_delete_();
        }

        int32_t instanceId() const override {
            return instance_id_;
        }

        const std::string& nodeId() const override {
            return node_id_;
        }

        const std::string& nodeType() const override {
            return node_type_;
        }

        const std::string& displayName() const override {
            static const std::string empty{};
            return instance_ ? instance_->displayName() : empty;
        }

        bool bypassed() const override {
            return instance_ && instance_->bypassed();
        }

        void bypassed(bool value) override {
            if (instance_)
                instance_->bypassed(value);
        }

        AudioPluginInstanceAPI* instance() override {
            return instance_;
        }

        int32_t processAudio(AudioProcessContext& process) override {
            return instance_ ? instance_->processAudio(process) : 0;
        }

        uint32_t latencyInSamples() const override {
            return instance_ ? instance_->latencyInSamples() : 0;
        }

        double tailLengthInSeconds() const override {
            return instance_ ? instance_->tailLengthInSeconds() : 0.0;
        }

        remidy::PluginAudioBuses* audioBuses() override {
            return instance_ ? instance_->audioBuses() : nullptr;
        }

        std::function<void()> releaseOnDelete() {
            return std::move(on_delete_);
        }

        ParameterUpdateEvent& parameterUpdateEvent() override {
            return parameter_update_event_;
        }

        ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() override {
            return parameter_metadata_refresh_event_;
        }

        // Called from non-RT thread. Enqueues UMP events.
        bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) override {
#ifdef __EMSCRIPTEN__
            if (instance_) {
                if (trySendWebClapInputEvents(instance_, static_cast<const uapmd_ump_t*>(events), size))
                    return true;
            }
#endif

            auto* bytes = static_cast<const uint8_t*>(events);
            size_t offset = 0;
            while (offset + sizeof(uint32_t) <= size) {
                auto* words = reinterpret_cast<const uint32_t*>(bytes + offset);
                auto messageType = static_cast<uint8_t>(words[0] >> 28);
                auto wordCount = umppi::umpSizeInInts(messageType);
                auto messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
                if (offset + messageSize > size)
                    break;
                umppi::Ump u128(words[0],
                                wordCount > 1 ? words[1] : 0,
                                wordCount > 2 ? words[2] : 0,
                                wordCount > 3 ? words[3] : 0);
                if (!queue_.enqueue(u128))
                    return false;
                offset += messageSize;
            }
            return true;
        }

        // Called from non-RT thread. Best-effort: emits MIDI All Sound Off on all channels.
        void sendAllNotesOff() override {
            for (uint8_t channel = 0; channel < 16; ++channel) {
                umppi::Ump all_sound_off(umppi::UmpFactory::midi2CC(0, channel, 120, 0));
                if (!queue_.try_enqueue(all_sound_off))
                    break;
            }
        }

        void requestStopFlush() override {
            stop_flush_requested_.store(true, std::memory_order_release);
        }

        // Internal methods for AudioPluginGraph to use (called from RT thread)

        void drainQueueToPending() {
            umppi::Ump u128;
            while (queue_.try_dequeue(u128))
                pending_events_.push_back(u128);
        }

        void drainPresetRequests() {
            if (ump_input_mapper_)
                ump_input_mapper_->drainPresetRequests();
        }

        void processInputMapping(AudioProcessContext& process) {
            if (auto* mapper = ump_input_mapper_.get())
                mapper->process(process);
        }

        bool consumeStopFlushRequest() {
            return stop_flush_requested_.exchange(false, std::memory_order_acq_rel);
        }

        size_t fillEventBufferForGroup(EventSequence& eventIn, uint8_t group) {
            auto* messages = static_cast<uint8_t*>(eventIn.getMessages());
            size_t position = eventIn.position();
            const auto capacity = eventIn.maxMessagesInBytes();

            auto it = pending_events_.begin();
            while (it != pending_events_.end()) {
                const auto& ump = *it;
                const auto msgGroup = ump.getGroup();
                if (group != 0xFF && msgGroup != group) {
                    ++it;
                    continue;
                }
                const auto messageSize = static_cast<size_t>(ump.getSizeInBytes());
                if (position + messageSize > capacity)
                    break;
                auto ints = ump.toInts();
                std::memcpy(messages + position, ints.data(), messageSize);
                position += messageSize;
                it = pending_events_.erase(it);
            }
            eventIn.position(position);
            return position;
        }

        void clearQueuedEvents() override {
            pending_events_.clear();
            umppi::Ump u128;
            while (queue_.try_dequeue(u128)) {
            }
        }

        // Scan the finalized per-cycle input event buffer and keep the active-note
        // bitmask in sync. Called from the RT thread right before the plugin processes.
        void trackInputEvents(EventSequence& eventIn) {
            const auto* words = static_cast<const uint32_t*>(eventIn.getMessages());
            const size_t count = eventIn.position() / sizeof(uint32_t);
            size_t i = 0;
            while (i < count) {
                const auto messageType = static_cast<uint8_t>(words[i] >> 28);
                size_t wordCount = umppi::umpSizeInInts(messageType);
                if (wordCount == 0) wordCount = 1;
                if (i + wordCount > count)
                    break;
                if (messageType == umppi::MidiMessageType::MIDI2 ||
                    messageType == umppi::MidiMessageType::MIDI1) {
                    const auto group = static_cast<uint8_t>((words[i] >> 24) & 0xF);
                    const auto status = static_cast<uint8_t>((words[i] >> 20) & 0xF) << 4;
                    const auto channel = static_cast<uint8_t>((words[i] >> 16) & 0xF);
                    const auto note = static_cast<uint8_t>((words[i] >> 8) & 0x7F);
                    // MIDI 2.0 note-on with velocity 0 is a real note-on; only MIDI 1.0
                    // treats it as a note-off.
                    const bool midi1ZeroVelocity = messageType == umppi::MidiMessageType::MIDI1 &&
                        (words[i] & 0x7Fu) == 0;
                    if (status == umppi::MidiChannelStatus::NOTE_ON && !midi1ZeroVelocity) {
                        active_notes_[group][channel][note >> 6] |= 1ull << (note & 63);
                        has_active_notes_ = true;
                    } else if (status == umppi::MidiChannelStatus::NOTE_OFF ||
                               (status == umppi::MidiChannelStatus::NOTE_ON && midi1ZeroVelocity)) {
                        active_notes_[group][channel][note >> 6] &= ~(1ull << (note & 63));
                    }
                }
                i += wordCount;
            }
        }

        void prepareStopFlush(EventSequence& eventIn, uint8_t group) {
            clearQueuedEvents();
            eventIn.position(0);
            bool flushComplete = true;

            // Emit a genuine note-off for every note still held. This is the only
            // portable way to release voices: e.g. VST3 has no All Sound Off event,
            // and CC 120 below only works when the plugin maps it via IMidiMapping.
            if (has_active_notes_) {
                for (uint8_t noteGroup = 0; noteGroup < 16 && flushComplete; ++noteGroup) {
                    for (uint8_t channel = 0; channel < 16 && flushComplete; ++channel) {
                        for (uint8_t half = 0; half < 2 && flushComplete; ++half) {
                            auto& bits = active_notes_[noteGroup][channel][half];
                            while (bits) {
                                const auto bit = static_cast<uint8_t>(std::countr_zero(bits));
                                const auto note = static_cast<uint8_t>(half * 64 + bit);
                                const uint64_t ump64 = umppi::UmpFactory::midi2NoteOff(
                                    noteGroup, channel, note, 0, 0x8000, 0);
                                const uapmd_ump_t words[2]{
                                    static_cast<uapmd_ump_t>(ump64 >> 32),
                                    static_cast<uapmd_ump_t>(ump64 & 0xFFFFFFFFu)
                                };
                                if (!appendUmpToEventBuffer(eventIn, words, 2)) {
                                    flushComplete = false;
                                    break;
                                }
                                bits &= bits - 1;
                            }
                        }
                    }
                }
                if (flushComplete)
                    has_active_notes_ = false;
            }

            const uint8_t flushGroup = group == 0xFF ? 0 : group;
            for (uint8_t channel = 0; channel < 16 && flushComplete; ++channel) {
                const uint64_t ump64 = umppi::UmpFactory::midi2CC(flushGroup, channel, 120, 0);
                const uapmd_ump_t words[2]{
                    static_cast<uapmd_ump_t>(ump64 >> 32),
                    static_cast<uapmd_ump_t>(ump64 & 0xFFFFFFFFu)
                };
                if (!appendUmpToEventBuffer(eventIn, words, 2)) {
                    flushComplete = false;
                    break;
                }
            }
            if (!flushComplete)
                stop_flush_requested_.store(true, std::memory_order_release);
        }

        friend class AudioPluginGraphImpl;

    private:
        static bool appendUmpToEventBuffer(EventSequence& eventIn, const uapmd_ump_t* words, size_t wordCount) {
            auto* messages = static_cast<uint8_t*>(eventIn.getMessages());
            size_t position = eventIn.position();
            const auto capacity = eventIn.maxMessagesInBytes();
            const auto messageSize = wordCount * sizeof(uint32_t);
            if (position + messageSize > capacity)
                return false;
            std::memcpy(messages + position, words, messageSize);
            eventIn.position(position + messageSize);
            return true;
        }
    };

}
