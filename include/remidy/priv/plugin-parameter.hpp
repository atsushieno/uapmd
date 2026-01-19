#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace remidy {

    // Parameters should be identified by two distinct ways:
    // - index in `parameters()`. It is RT-safe but not stable (not persistable).
    // - `id()` by std::string. It cannot be used in RT-safe code but stable.
    struct ParameterEnumeration {
        std::string label;
        double value;
        ParameterEnumeration(std::string& label, double value) : label(label), value(value) {}
    };

    class PluginParameter {
        const uint32_t _index;
        const std::string stable_id;
        const std::string _name;
        const std::string _path;
        double default_plain_value, min_plain_value, max_plain_value;
        bool is_automatable;
        bool is_readable;
        bool is_hidden;
        bool is_discrete;
        const std::vector<ParameterEnumeration> _enums;

    public:
        PluginParameter(uint32_t index, std::string& id, std::string& name, std::string& path,
                        double defaultPlainValue, double minPlainValue, double maxPlainValue,
                        bool automatable, bool readable, bool hidden, bool discrete,
                        std::vector<ParameterEnumeration> enums = {}) :
                _index(index), stable_id(id), _name(name), _path(path),
                default_plain_value(defaultPlainValue), min_plain_value(minPlainValue), max_plain_value(maxPlainValue),
                is_automatable(automatable), is_readable(readable), is_hidden(hidden), is_discrete(discrete), _enums(std::move(enums)) {
        }
        ~PluginParameter() = default;

        // Index in a PluginInstance.
        // Note that it is NOT stable. Next time you load the "same" plugin, the ID may be different.
        const uint32_t index() const { return _index; }
        const std::string& stableId() const { return stable_id; }
        const std::string& name() const { return _name; }
        // If the format only supports "group", then it will be "group".
        // If the format supports parameter path, then it will be "path/to/sub/path"
        const std::string& path() const { return _path; }
        const double defaultPlainValue() const { return default_plain_value; }
        const double minPlainValue() const { return min_plain_value; }
        const double maxPlainValue() const { return max_plain_value; }
        double normalizedValue(double plainValue) const { return (plainValue - min_plain_value) / (max_plain_value - min_plain_value); }
        bool automatable() const { return is_automatable; }
        bool readable() { return is_readable; }
        bool hidden() const { return is_hidden; }
        bool discrete() const { return is_discrete; }
        // FIXME: should we make it const as well?
        void readable(bool newValue) { is_readable = newValue; }
        const std::vector<ParameterEnumeration>& enums() { return _enums; }

        // Update parameter range (for plugins that change min/max dynamically)
        void updateRange(double newMin, double newMax, double newDefault) {
            min_plain_value = newMin;
            max_plain_value = newMax;
            default_plain_value = newDefault;
        }
    };

    // any combination of these values
    enum PerNoteControllerContextTypes {
        // no distinct list of per-note controllers from global parameters
        PER_NOTE_CONTROLLER_NONE = 0,
        // distinct list of PNCs per channel
        PER_NOTE_CONTROLLER_PER_CHANNEL = 1,
        // distinct list of PNCs per MIDI note
        PER_NOTE_CONTROLLER_PER_NOTE = 2,
        // distinct list of PNCs per UMP group, or bus, kind of
        PER_NOTE_CONTROLLER_PER_GROUP = 4,
        // distinct list of PNCs per extra field
        PER_NOTE_CONTROLLER_PER_EXTRA = 8,
    };

    // Each plugin format has distinct way to provide set of per-note controllers:
    // - VST3 may return distinct set of controllers per channel
    // - AU may return distinct set of controllers per note xor per channel
    // - LV2 does not support per-note controllers.
    // - CLAP does not have separate set of parameters from global (non-per-note) parameters.
    struct PerNoteControllerContext {
        uint32_t note;
        uint32_t channel;
        uint32_t group;
        uint32_t extra; // for any future uses
    };

    using EventListenerId = uint64_t;

    template<typename TReturn, typename ...TArgs>
    class EventSupport {
        friend class PluginParameterSupport;

    public:
        using EventListener = std::function<TReturn(TArgs...)>;

    private:
        std::atomic<EventListenerId> listenerIdCounter{1};
        std::unordered_map<EventListenerId, EventListener> listeners{};
        std::mutex listenerMutex{};

        void notify(TArgs... args) {
            std::vector<EventListener> callbacks;
            {
                std::lock_guard<std::mutex> lock(listenerMutex);
                callbacks.reserve(listeners.size());
                for (auto& kv : listeners)
                    callbacks.emplace_back(kv.second);
            }
            for (auto& cb : callbacks)
                if (cb)
                    cb(args...);
        }

    public:
        EventListenerId addListener(EventListener listener) {
            if (!listener)
                return 0;
            std::lock_guard<std::mutex> lock(listenerMutex);
            auto id = listenerIdCounter++;
            listeners.emplace(id, std::move(listener));
            return id;
        }

        void removeListener(EventListenerId id) {
            if (id == 0)
                return;
            std::lock_guard<std::mutex> lock(listenerMutex);
            listeners.erase(id);
        }
    };

    using ParameterMetadataChangeEventSupport = EventSupport<void>;
    using ParameterChangeEventSupport = EventSupport<void, uint32_t, double>;

    class PluginParameterSupport {
        ParameterMetadataChangeEventSupport parameterMetadataChangeEvent{};
        ParameterChangeEventSupport parameterChangeEvent{};

    protected:
        void notifyParameterChangeListeners(uint32_t index, double plainValue) {
            parameterChangeEvent.notify(index, plainValue);
        }

        void notifyParameterMetadataChangeListeners() {
            parameterMetadataChangeEvent.notify();
        }

    public:
        virtual ~PluginParameterSupport() = default;

        EventListenerId addParameterChangeListener(ParameterChangeEventSupport::EventListener&& listener) {
            return parameterChangeEvent.addListener(std::move(listener));
        }

        void removeParameterChangeListener(EventListenerId id) {
            parameterChangeEvent.removeListener(id);
        }

        EventListenerId addParameterMetadataChangeListener(ParameterMetadataChangeEventSupport::EventListener&& listener) {
            return parameterMetadataChangeEvent.addListener(std::move(listener));
        }

        void removeParameterMetadataChangeListener(EventListenerId id) {
            parameterMetadataChangeEvent.removeListener(id);
        }

        // Returns the list of parameter metadata.
        virtual std::vector<PluginParameter*>& parameters() = 0;

        // Returns the list of per-note controller metadata.
        // They are *different* from `parameters()` in VST3 and AU (also in MIDI 2.0).
        // In CLAP there is no distinct list of parameters for per-note controllers,
        // so it should return the same list.
        //
        // Usually the `note` argument does not matter (and *should* not), but it might.
        // See `perNoteControllerDefinitionsMayBeDistinctPerNote()` for details.
        virtual std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext context) = 0;

        // Sets (schedules) a normalized parameter value by index.
        virtual StatusCode setParameter(uint32_t index, double plainValue, uint64_t timestamp) = 0;
        // Retrieves current parameter, if possible.
        virtual StatusCode getParameter(uint32_t index, double *plainValue) = 0;

        // Sets (schedules) a normalized per-note controller value by index.
        // covers both parameter changes and per-note parameter changes (controllers).
        // Note that only some plugin formats support per-note controllers beyond 127.
        virtual StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) = 0;

        // Retrieves current per-note controller value, if possible.
        virtual StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) = 0;

        // Gets string representation of a parameter value, which might be enumerated or specially labeled.
        //
        // In some plugin formats, enumerated values might not be exposed as enumerations.
        // Those plugins make it impossible to query defined values in prior and do not give the best user experience,
        // but that's not our fault...
        virtual std::string valueToString(uint32_t index, double value) = 0;
        virtual std::string valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) = 0;

        // Refresh parameter metadata (min/max/default values) from the plugin.
        // This is needed for plugins that may change parameter ranges dynamically.
        virtual void refreshParameterMetadata(uint32_t index) {}

        // Refresh all parameter metadata.
        virtual void refreshAllParameterMetadata() {
            auto& params = parameters();
            for (size_t i = 0; i < params.size(); i++)
                refreshParameterMetadata(static_cast<uint32_t>(i));
            notifyParameterMetadataChangeListeners();
        }
    };
}
