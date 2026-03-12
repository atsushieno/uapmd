#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

} // namespace uapmd
