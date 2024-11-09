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
        const std::string _id;
        const std::string _name;
        const std::string _path;
        const double default_value, min_value, max_value;
        bool is_readable;
        const std::vector<ParameterEnumeration> _enums;

    public:
        PluginParameter(std::string& id, std::string& name, std::string& path,
                        double defaultValue, double minValue, double maxValue,
                        bool readable,
                        std::vector<ParameterEnumeration> enums = {}) :
            _id(id), _name(name), _path(path),
            default_value(defaultValue), min_value(minValue), max_value(maxValue),
            is_readable(readable), _enums(enums) {
        }
        ~PluginParameter() = default;

        const std::string& id() const { return _id; }
        const std::string& name() const { return _name; }
        // If the format only supports "group", then it will be "group".
        // If the format supports parameter path, then it will be "path/to/sub/path"
        const std::string& path() { return _path; }
        const double defaultValue() const { return default_value; }
        const double minValue() const { return min_value; }
        const double maxValue() const { return max_value; }
        bool readable() { return is_readable; }
        void readable(bool newValue) { is_readable = newValue; }
        const std::vector<ParameterEnumeration>& enums() { return _enums; }
    };

    class PluginParameterSupport {
    public:
        virtual std::vector<PluginParameter*> parameters() = 0;
        virtual StatusCode setParameter(uint32_t index, double value) = 0;
        virtual StatusCode getParameter(uint32_t index, double *value) = 0;
    };
}