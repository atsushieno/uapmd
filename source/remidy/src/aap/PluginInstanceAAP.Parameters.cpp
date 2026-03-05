#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <sstream>
#include <umppi/umppi.hpp>

#include "remidy/remidy.hpp"
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

remidy::PluginInstanceAAP::ParameterSupport::ParameterSupport(PluginInstanceAAP *owner) : owner(owner) {
    auto aap = owner->aapInstance();
    for (auto i = 0, n = aap->getNumParameters(); i < n; i++) {
        auto src = aap->getParameter(i);
        std::vector<remidy::ParameterEnumeration> enums{};
        for (auto e = 0, ne = src->getEnumCount(); e < ne; e++) {
            auto en = src->getEnumeration(e);
            std::string name{en.getName()};
            remidy::ParameterEnumeration ed{name, en.getValue()};
            enums.push_back(ed);
        }
        std::string id = std::to_string(src->getId());
        std::string name = src->getName();
        std::string path = "";
        parameter_list.push_back(new PluginParameter(i, id, name, path,
                                                     src->getDefaultValue(),
                                                     src->getMinimumValue(),
                                                     src->getMaximumValue(),
                                                     true,
                                                     true,
                                                     false,
                                                     src->getEnumCount() > 0,
                                                     enums));
        parameter_values.push_back(src->getDefaultValue());

        // AAP does not have per-note controller yet.
    }
}

remidy::PluginInstanceAAP::ParameterSupport::~ParameterSupport() {
    for (auto p : parameter_list)
        delete p;
    for (auto p : per_note_controller_list)
        delete p;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::setParameter(uint32_t index, double plainValue,
                                                uint64_t timestamp) {
    if (index >= parameter_list.size())
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto* param = parameter_list[index];
    double clamped = std::clamp(plainValue, param->minPlainValue(), param->maxPlainValue());
    const double normalized = std::clamp(param->normalizedValue(clamped), 0.0, 1.0);
    parameter_values[index] = clamped;

    // Convert to MIDI 2.0 NRPN per AAP parameter mapping (bank = index >> 7, controller = index & 0x7F)
    constexpr uint8_t group = 0;
    constexpr uint8_t channel = 0;
    const uint8_t bankMsb = static_cast<uint8_t>((index >> 7) & 0x7F);
    const uint8_t controller = static_cast<uint8_t>(index & 0x7F);
    const auto data = static_cast<uint32_t>(normalized * static_cast<double>(std::numeric_limits<uint32_t>::max()));
    auto umpValue = umppi::UmpFactory::midi2NRPN(group, channel, bankMsb, controller, data);
    const uint32_t words[2]{
        static_cast<uint32_t>(umpValue >> 32),
        static_cast<uint32_t>(umpValue & 0xFFFFFFFFu)
    };

    owner->aapInstance()->addEventUmpInput((void*) words, sizeof(words));

    parameterChangeEvent().notify(index, clamped);
    return StatusCode::OK;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::getParameter(uint32_t index, double *plainValue) {
    if (!plainValue || index >= parameter_values.size())
        return StatusCode::INVALID_PARAMETER_OPERATION;
    *plainValue = parameter_values[index];
    return StatusCode::OK;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::setPerNoteController(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double value,
                                                        uint64_t timestamp) {
    return StatusCode::INVALID_PARAMETER_OPERATION;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::getPerNoteController(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double *value) {
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    *value = 0.0;
    return StatusCode::INVALID_PARAMETER_OPERATION;
}

std::string remidy::PluginInstanceAAP::ParameterSupport::valueToString(uint32_t index, double value) {
    if (index >= parameter_list.size())
        return {};
    const auto& enums = parameter_list[index]->enums();
    if (enums.empty()) {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(3);
        oss << value;
        return oss.str();
    }
    const double tolerance = 1e-6;
    const ParameterEnumeration* bestMatch = nullptr;
    double bestDiff = std::numeric_limits<double>::max();
    for (const auto& e : enums) {
        const double diff = std::abs(e.value - value);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestMatch = &e;
        }
    }
    if (!bestMatch)
        return {};
    if (bestDiff <= tolerance)
        return bestMatch->label;
    if (value <= enums.front().value)
        return enums.front().label;
    if (value >= enums.back().value)
        return enums.back().label;
    return bestMatch->label;
}

std::string
remidy::PluginInstanceAAP::ParameterSupport::valueToStringPerNote(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double value) {
    return valueToString(index, value);
}

void remidy::PluginInstanceAAP::ParameterSupport::ingestPluginParameterUpdates(const uint8_t *data, size_t lengthInBytes) {
    size_t offset = 0;
    while (offset + sizeof(uint32_t) <= lengthInBytes) {
        auto* words = reinterpret_cast<const uint32_t*>(data + offset);
        auto messageType = static_cast<uint8_t>(words[0] >> 28);
        auto wordCount = umppi::umpSizeInInts(messageType);
        size_t messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
        if (offset + messageSize > lengthInBytes)
            break;
        umppi::Ump ump(words[0],
                       wordCount > 1 ? words[1] : 0,
                       wordCount > 2 ? words[2] : 0,
                       wordCount > 3 ? words[3] : 0);
        if (ump.getMessageType() == umppi::MessageType::MIDI2) {
            switch (static_cast<uint8_t>(ump.getStatusCode())) {
                case umppi::MidiChannelStatus::NRPN:
                case umppi::MidiChannelStatus::RELATIVE_NRPN: {
                    const auto bank = ump.getMidi2NrpnMsb();
                    const auto controller = ump.getMidi2NrpnLsb();
                    const uint32_t parameterIndex = bank * 0x80 + controller;
                    if (parameterIndex < parameter_list.size()) {
                        const double normalized = static_cast<double>(ump.getMidi2NrpnData()) /
                                                  static_cast<double>(std::numeric_limits<uint32_t>::max());
                        auto* param = parameter_list[parameterIndex];
                        const double plain = param->minPlainValue() +
                                             normalized * (param->maxPlainValue() - param->minPlainValue());
                        parameter_values[parameterIndex] = plain;
                        parameterChangeEvent().notify(parameterIndex, plain);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        offset += messageSize;
    }
}
