#pragma once

#include <charconv>
#include <format>
#include <stdexcept>
#include <choc/containers/choc_Value.h>
#include "ScriptDocumentIds.hpp"

namespace uapmd_app::script_track_properties {

    // Native exceptions must not unwind through QuickJS's C stack. The public
    // JS wrapper turns this error value into a JavaScript exception instead.
    template<typename Function>
    choc::value::Value invokeForJavaScript(Function&& function) {
        try {
            return function();
        } catch (const std::exception& exception) {
            auto result = choc::value::createObject("TrackPropertiesError");
            result.setMember("error", std::string(exception.what()));
            return result;
        }
    }

    inline choc::value::Value serialize(const uapmd::TrackProperties& properties) {
        auto result = choc::value::createObject("TrackProperties");
        result.setMember("color", properties.color
            ? choc::value::createString(std::format("#{:06X}", *properties.color))
            : choc::value::Value{});
        result.setMember("path", properties.path);
        result.setMember("order", properties.order);
        return result;
    }

    inline uapmd::ProjectTrackSnapshot resolve(std::string_view trackId) {
        auto track = script_ids::document().view().getTrack(std::string(trackId));
        if (!track)
            throw std::invalid_argument("Unknown trackId: " + std::string(trackId));
        return *track;
    }

    inline choc::value::Value get(std::string_view trackId) {
        return serialize(resolve(trackId).properties);
    }

    // Validate the whole patch before submitting one undoable document command.
    inline choc::value::Value set(std::string_view trackId, const choc::value::ValueView& patch) {
        const auto track = resolve(trackId);
        auto properties = track.properties;
        if (!patch.isObject())
            throw std::invalid_argument("properties must be an object");
        for (uint32_t i = 0; i < patch.size(); ++i) {
            const std::string_view name = patch.getObjectMemberAt(i).name;
            if (name != "color" && name != "path" && name != "order")
                throw std::invalid_argument("Unknown track property: " + std::string(name));
        }
        if (patch.hasObjectMember("color")) {
            const auto value = patch["color"];
            if (value.isVoid())
                properties.color.reset();
            else {
                if (!value.isString())
                    throw std::invalid_argument("color must be #RRGGBB or null");
                const std::string color(value.getString());
                if (color.size() != 7 || color.front() != '#')
                    throw std::invalid_argument("color must be #RRGGBB or null");
                uint32_t rgb{};
                const auto parsed = std::from_chars(color.data() + 1, color.data() + color.size(), rgb, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != color.data() + color.size())
                    throw std::invalid_argument("color must be #RRGGBB or null");
                properties.color = rgb;
            }
        }
        if (patch.hasObjectMember("path")) {
            if (!patch["path"].isString())
                throw std::invalid_argument("path must be a string");
            properties.path = patch["path"].getString();
        }
        if (patch.hasObjectMember("order")) {
            if (!patch["order"].isInt() && !patch["order"].isFloat())
                throw std::invalid_argument("order must be a finite number");
            properties.order = patch["order"].get<double>();
            if (!properties.valid())
                throw std::invalid_argument("order must be a finite number");
        }
        if (!script_ids::document().commands().setTrackProperties(
                track.trackIndex, properties, uapmd::ProjectMutationOrigin::Remote))
            throw std::runtime_error("Could not update track properties");
        return serialize(properties);
    }

}
