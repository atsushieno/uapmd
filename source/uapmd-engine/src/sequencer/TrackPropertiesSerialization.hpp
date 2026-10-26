#pragma once

#include <charconv>
#include <format>
#include <map>
#include <unordered_set>
#include <choc/text/choc_JSON.h>
#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd::timeline_detail {

    // Kept outside the MIDI/container document, using the package's extension API.
    class TrackPropertiesSerialization final : public ProjectSerializationExtension {
        TimelineFacade& timeline_;
        std::unordered_set<std::string> saved_tracks_;

    public:
        explicit TrackPropertiesSerialization(TimelineFacade& timeline) : timeline_(timeline) {}

        std::string_view extensionId() const override { return "track-properties"; }

        bool saveProjectData(UapmdProjectData& project, std::string&) override {
            saved_tracks_.clear();
            for (const auto* track : project.tracks())
                saved_tracks_.insert(track->referenceId());
            if (const auto* master = project.masterTrack())
                saved_tracks_.insert(master->referenceId());
            return true;
        }

        bool saveProjectExtensionData(ProjectSerializationWriteContext& context, std::string& error) override {
            auto root = choc::value::createObject("TrackMetadata");
            root.addMember("version", 1);
            auto entries = choc::value::createObject("Tracks");
            auto tracks = timeline_.tracks();
            tracks.push_back(timeline_.masterTimelineTrack());
            for (const auto* track : tracks) {
                if (!track || !saved_tracks_.contains(track->referenceId()))
                    continue;
                const auto& properties = track->properties();
                auto entry = choc::value::createObject("TrackProperties");
                if (properties.color)
                    entry.addMember("color", std::format("#{:06X}", *properties.color));
                entry.addMember("path", properties.path);
                entry.addMember("order", properties.order);
                entries.addMember(track->referenceId(), entry);
            }
            root.addMember("tracks", entries);
            const auto json = choc::json::toString(root, true);
            return context.writeExtensionFile(extensionId(), "tracks.json",
                std::vector<uint8_t>(json.begin(), json.end()), error);
        }

        bool loadProjectExtensionData(ProjectSerializationReadContext& context, std::string& error) override {
            // Absence is normal for older projects. Other read failures are reported.
            std::error_code ec;
            const auto file = context.projectDirectory() / "extensions" / "track-properties" / "tracks.json";
            const bool exists = std::filesystem::exists(file, ec);
            if (ec) {
                error = ec.message();
                return false;
            }
            if (!exists)
                return true;
            const auto bytes = context.readExtensionFile(extensionId(), "tracks.json", error);
            if (!bytes)
                return false;

            try {
                const auto root = choc::json::parse(std::string(bytes->begin(), bytes->end()));
                if (!root.isObject() || !root.hasObjectMember("version")
                    || !root["version"].isInt() || root["version"].getInt64() != 1
                    || !root.hasObjectMember("tracks") || !root["tracks"].isObject()) {
                    error = "Unsupported track metadata format.";
                    return false;
                }
                const auto entries = root["tracks"];
                auto tracks = timeline_.tracks();
                tracks.push_back(timeline_.masterTimelineTrack());
                std::map<TimelineTrack*, TrackProperties> parsed;
                for (auto* track : tracks) {
                    if (!track || !entries.hasObjectMember(track->referenceId()))
                        continue;
                    const auto entry = entries[track->referenceId()];
                    auto properties = track->properties();
                    if (!entry.isObject())
                        throw std::runtime_error("Expected a track properties object.");
                    if (entry.hasObjectMember("path")) {
                        if (!entry["path"].isString())
                            throw std::runtime_error("Track path must be a string.");
                        properties.path = entry["path"].getString();
                    }
                    if (entry.hasObjectMember("order")) {
                        if (!entry["order"].isInt() && !entry["order"].isFloat())
                            throw std::runtime_error("Track order must be a finite number.");
                        properties.order = entry["order"].get<double>();
                    }
                    if (entry.hasObjectMember("color")) {
                        if (!entry["color"].isString())
                            throw std::runtime_error("Track color must be #RRGGBB.");
                        const std::string color(entry["color"].getString());
                        uint32_t rgb{};
                        const auto result = color.size() == 7 && color[0] == '#'
                            ? std::from_chars(color.data() + 1, color.data() + color.size(), rgb, 16)
                            : std::from_chars_result{color.data(), std::errc::invalid_argument};
                        if (result.ec != std::errc{} || result.ptr != color.data() + color.size())
                            throw std::runtime_error("Track color must be #RRGGBB.");
                        properties.color = rgb;
                    }
                    if (!properties.valid())
                        throw std::runtime_error("Invalid track properties.");
                    parsed.emplace(track, std::move(properties));
                }
                for (const auto& [track, properties] : parsed)
                    track->properties(properties);
                return true;
            } catch (const std::exception& exception) {
                error = exception.what();
                return false;
            }
        }
    };

}
