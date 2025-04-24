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
        bool is_readable;
        bool is_hidden;
        const std::vector<ParameterEnumeration> _enums;

    public:
        PluginParameter(uint32_t index, std::string& id, std::string& name, std::string& path,
                        double defaultValue, double minValue, double maxValue,
                        bool readable, bool hidden,
                        std::vector<ParameterEnumeration> enums = {}) :
            _index(index), stable_id(id), _name(name), _path(path),
            default_value(defaultValue), min_value(minValue), max_value(maxValue),
            is_readable(readable), is_hidden(hidden), _enums(enums) {
        }
        ~PluginParameter() = default;

        const uint32_t index() const { return _index; }
        const std::string& stableId() const { return stable_id; }
        const std::string& name() const { return _name; }
        // If the format only supports "group", then it will be "group".
        // If the format supports parameter path, then it will be "path/to/sub/path"
        const std::string& path() const { return _path; }
        const double defaultValue() const { return default_value; }
        const double minValue() const { return min_value; }
        const double maxValue() const { return max_value; }
        bool readable() { return is_readable; }
        bool hidden() const { return is_hidden; }
        // FIXME: should we make it const as well?
        void readable(bool newValue) { is_readable = newValue; }
        const std::vector<ParameterEnumeration>& enums() { return _enums; }
    };

    class PluginParameterSupport {
    public:
        // True or false depending on the plugin format.
        // True for VST3 and CLAP, false for AU and LV2.
        virtual bool accessRequiresMainThread() = 0;

        // Returns the list of parameter metadata.
        virtual std::vector<PluginParameter*> parameters() = 0;

        // Sets (schedules) a normalized parameter value by index.
        // `note` should be < 0 if the request is not for a per-note controller.
        // covers both parameter changes and per-note parameter changes (controllers).
        // Note that only some plugin formats support per-note controllers beyond 127.
        virtual StatusCode setParameter(int32_t note, uint32_t index, double value, uint64_t timestamp) = 0;

        // Retrieves current parameter, if possible.
        // `note` should be < 0 if the request is not for a per-note controller.
        virtual StatusCode getParameter(int32_t note, uint32_t index, double *value) = 0;
    };
}