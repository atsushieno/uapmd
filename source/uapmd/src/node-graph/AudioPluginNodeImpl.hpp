#pragma once
#include <umppi/umppi.hpp>
#include "concurrentqueue.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include "uapmd/uapmd.hpp"
#include "../midi/UapmdNodeUmpMapper.hpp"

namespace uapmd {

    class AudioPluginNodeImpl : public AudioPluginNode {
        int32_t instance_id_;
        AudioPluginInstanceAPI* instance_;
        moodycamel::ConcurrentQueue<umppi::Ump> queue_;
        std::vector<umppi::Ump> pending_events_;
        std::function<void()> on_delete_;
        ParameterUpdateEvent parameter_update_event_;
        ParameterMetadataRefreshEvent parameter_metadata_refresh_event_;
        remidy::EventListenerId parameter_listener_token_{0};
        remidy::EventListenerId metadata_listener_token_{0};
        // active_notes_ is only accessed from non-RT paths (scheduleEvents, sendAllNotesOff).
        std::unordered_map<uint16_t, uint32_t> active_notes_;
        std::unique_ptr<UapmdNodeUmpInputMapper> ump_input_mapper_{};

        struct PendingNoteUpdate {
            bool note_on;
            uint16_t key;
        };

    public:
        AudioPluginNodeImpl(
            int32_t instanceId,
            AudioPluginInstanceAPI* instance,
            size_t eventBufferSizeInBytes,
            std::function<void()>&& onDelete
        ) : instance_id_(instanceId),
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

        AudioPluginInstanceAPI* instance() override {
            return instance_;
        }

        ParameterUpdateEvent& parameterUpdateEvent() override {
            return parameter_update_event_;
        }

        ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() override {
            return parameter_metadata_refresh_event_;
        }

        // Called from non-RT thread. Enqueues UMP events and tracks note state locally.
        bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) override {
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
                PendingNoteUpdate note_update{};
                if (extractNoteUpdate(u128, note_update))
                    applyNoteUpdate(note_update);
                if (!queue_.enqueue(u128))
                    return false;
                offset += messageSize;
            }
            return true;
        }

        // Called from non-RT thread. Best-effort: skips notes that can't be enqueued.
        void sendAllNotesOff() override {
            if (active_notes_.empty())
                return;
            for (auto it = active_notes_.begin(); it != active_notes_.end(); ) {
                const auto note = decodeNoteKey(it->first);
                auto remaining = it->second;
                uint32_t sent = 0;
                while (sent < remaining) {
                    umppi::Ump note_off(umppi::UmpFactory::midi2NoteOff(note.group, note.channel, note.note, 0, 0, 0));
                    if (!queue_.try_enqueue(note_off))
                        break;
                    ++sent;
                }
                if (sent == remaining)
                    it = active_notes_.erase(it);
                else {
                    it->second -= sent;
                    ++it;
                }
            }
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

        friend class AudioPluginGraphImpl;

    private:
        static uint16_t encodeNoteKey(uint8_t group, uint8_t channel, uint8_t note) {
            const auto g = static_cast<uint16_t>(group & 0x0F);
            const auto ch = static_cast<uint16_t>(channel & 0x0F);
            const auto n = static_cast<uint16_t>(note & 0x7F);
            return static_cast<uint16_t>((g << 12) | (ch << 8) | n);
        }

        struct ActiveNoteInfo {
            uint8_t group;
            uint8_t channel;
            uint8_t note;
        };

        static ActiveNoteInfo decodeNoteKey(uint16_t key) {
            return ActiveNoteInfo{
                static_cast<uint8_t>((key >> 12) & 0x0F),
                static_cast<uint8_t>((key >> 8) & 0x0F),
                static_cast<uint8_t>(key & 0x7F)
            };
        }

        void applyNoteUpdate(const PendingNoteUpdate& update) {
            auto it = active_notes_.find(update.key);
            if (update.note_on) {
                if (it == active_notes_.end())
                    active_notes_[update.key] = 1;
                else
                    ++(it->second);
                return;
            }
            if (it == active_notes_.end())
                return;
            if (it->second <= 1)
                active_notes_.erase(it);
            else
                --(it->second);
        }

        static bool extractNoteUpdate(const umppi::Ump& ump, PendingNoteUpdate& update) {
            const auto messageType = ump.getMessageType();
            if (messageType != umppi::MessageType::MIDI1 &&
                messageType != umppi::MessageType::MIDI2)
                return false;

            const auto status = static_cast<uint8_t>(ump.getStatusCode());
            if (status != umppi::MidiChannelStatus::NOTE_ON &&
                status != umppi::MidiChannelStatus::NOTE_OFF)
                return false;

            const auto group = ump.getGroup();
            const auto channel = ump.getChannelInGroup();
            const uint8_t note = messageType == umppi::MessageType::MIDI1 ?
                ump.getMidi1Note() : ump.getMidi2Note();
            const auto key = encodeNoteKey(group, channel, note);

            if (status == umppi::MidiChannelStatus::NOTE_OFF) {
                update = {false, key};
                return true;
            }

            if (messageType == umppi::MessageType::MIDI1 && ump.getMidi1Velocity() == 0) {
                update = {false, key};
                return true;
            }

            if (messageType == umppi::MessageType::MIDI2 && ump.getMidi2Velocity16() == 0) {
                update = {false, key};
                return true;
            }

            update = {true, key};
            return true;
        }
    };

}
