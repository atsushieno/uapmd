#pragma once

#include <string>
#include <vector>
#include <ranges>

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
        const double default_value, min_value, max_value;
        bool is_automatable;
        bool is_readable;
        bool is_hidden;
        bool is_discrete;
        const std::vector<ParameterEnumeration> _enums;

    public:
        PluginParameter(uint32_t index, std::string& id, std::string& name, std::string& path,
                        double defaultValue, double minValue, double maxValue,
                        bool automatable, bool readable, bool hidden, bool discrete,
                        std::vector<ParameterEnumeration> enums = {}) :
            _index(index), stable_id(id), _name(name), _path(path),
            default_value(defaultValue), min_value(minValue), max_value(maxValue),
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
        const double defaultValue() const { return default_value; }
        const double minValue() const { return min_value; }
        const double maxValue() const { return max_value; }
        bool automatable() const { return is_automatable; }
        bool readable() { return is_readable; }
        bool hidden() const { return is_hidden; }
        bool discrete() const { return is_discrete; }
        // FIXME: should we make it const as well?
        void readable(bool newValue) { is_readable = newValue; }
        const std::vector<ParameterEnumeration>& enums() { return _enums; }
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

    class PluginParameterSupport {
    public:
        virtual ~PluginParameterSupport() = default;

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
        virtual StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) = 0;
        // Retrieves current parameter, if possible.
        virtual StatusCode getParameter(uint32_t index, double *value) = 0;

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
    };
}