#include "Augene2Compiler.hpp"
#include "BuiltinSources.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <mugene2/mugene2.hpp>

namespace uapmd_augene2 {
namespace {

std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    std::string text{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad())
        return {};
    return text;
}


}

Compilation compileSources(std::vector<Source> sources, bool refresh, bool onlyIfChanged) {
    Compilation result;
    result.sources = sources;
    try {
        if (refresh)
            for (auto& source : result.sources)
                if (!source.external_path.empty()) {
                    std::error_code error;
                    auto modified = std::filesystem::last_write_time(source.external_path, error);
                    if (onlyIfChanged && !error &&
                        std::filesystem::file_time_type::clock::now() - modified < std::chrono::milliseconds(500))
                        return result;
                    if (auto text = readText(source.external_path))
                        source.text = std::move(*text);
                    else
                        result.diagnostics.push_back("Source unavailable; using bundled copy: " + source.path);
                }

        if (onlyIfChanged && result.sources == sources)
            return result;
        result.attempted = true;

        std::vector<mugene2::SourceText> inputs;
        for (const auto* name : {"default-macro2.mml", "drum-part.mml", "gs-sysex.mml", "nrpn-gs-xg.mml"})
            inputs.push_back({name, builtin_sources.at(name)});
        bool has_input = false;
        for (const auto& source : result.sources)
            if (source.compile) {
                inputs.push_back({source.path, source.text});
                has_input = true;
            }
        if (!has_input)
            throw std::runtime_error("Import at least one MML compilation input.");

        auto resolver = [&](std::string_view including, std::string_view requested) -> std::optional<mugene2::SourceText> {
            std::string request(requested);
            if (request.size() >= 2 && request.front() == '"' && request.back() == '"')
                request = request.substr(1, request.size() - 2);
            const auto relative = (std::filesystem::path(including).parent_path() / request).lexically_normal();
            const auto key = relative.generic_string();
            if (relative.is_absolute() || key == ".." || key.starts_with("../"))
                return {};
            for (const auto& source : result.sources)
                if (source.path == key)
                    return mugene2::SourceText{key, source.text};
            if (auto builtin = builtin_sources.find(request); builtin != builtin_sources.end())
                return mugene2::SourceText{builtin->first, builtin->second};
            // Capture discovered includes into the returned bundle. Subsequent
            // offline compilations resolve these from the snapshot above.
            auto parent = std::ranges::find(result.sources, including, &Source::path);
            if (parent != result.sources.end() && !parent->external_path.empty()) {
                auto external = (std::filesystem::path(parent->external_path).parent_path() / request).lexically_normal();
                if (auto text = readText(external)) {
                    result.sources.push_back({key, *text, external.string(), false});
                    return mugene2::SourceText{key, std::move(*text)};
                }
            }
            return {};
        };
        mugene2::CompileOptions options;
        options.skip_default_mml_files = true;
        auto compiled = mugene2::compile_to_smf2clips(inputs, options, resolver);
        for (const auto& diagnostic : compiled.diagnostics)
            result.diagnostics.push_back(std::format("{}:{}:{}: {}", diagnostic.source_name,
                diagnostic.line, diagnostic.column, diagnostic.message));
        if (!compiled.success())
            return result;

        std::map<int64_t, double> tempos;
        std::map<int64_t, uapmd::MidiTimeSignatureChange> signatures;
        uint32_t resolution = 0;
        for (const auto& track : compiled.tracks) {
            if (!std::isfinite(track.source_track_number))
                throw std::runtime_error("Invalid MML track number.");
            // Preserve fractional labels exactly as the compiler distinguishes them.
            const auto track_key = std::format("{}", track.source_track_number);
            for (size_t index = 0; index < track.clips.size(); ++index) {
                const auto& clip = track.clips[index];
                auto content = uapmd::MidiClipReader::readSmf2Clip(clip.smf2clip);
                if (!content.success || content.tick_resolution == 0)
                    throw std::runtime_error("Invalid compiler clip: " + content.error);
                if (resolution && resolution != content.tick_resolution)
                    throw std::runtime_error("Compiler emitted inconsistent tick resolutions.");
                resolution = content.tick_resolution;
                auto absoluteTick = [&](uint64_t tick) {
                    return static_cast<int64_t>(tick) + clip.position_dctpq;
                };
                // Sequential settings at the same tick within one clip are
                // valid MML (for example an initial default followed by t145).
                // Use the final value before checking other clips for conflicts.
                std::map<int64_t, double> clip_tempos;
                std::map<int64_t, uapmd::MidiTimeSignatureChange> clip_signatures;
                if (content.has_explicit_tempo_changes)
                    for (const auto& tempo : content.tempo_changes)
                        clip_tempos[absoluteTick(tempo.tickPosition)] = tempo.bpm;
                if (content.has_explicit_time_signature_changes)
                    for (const auto& signature : content.time_signature_changes)
                        clip_signatures[absoluteTick(signature.tickPosition)] = signature;
                for (const auto& [tick, bpm] : clip_tempos) {
                    if (tempos.contains(tick) && tempos.at(tick) != bpm)
                        throw std::runtime_error("Conflicting MML tempo events at the same position.");
                    tempos[tick] = bpm;
                }
                for (const auto& [tick, signature] : clip_signatures) {
                    if (signatures.contains(tick) &&
                        (signatures.at(tick).numerator != signature.numerator || signatures.at(tick).denominator != signature.denominator))
                        throw std::runtime_error("Conflicting MML time signatures at the same position.");
                    signatures[tick] = signature;
                }
                // Compiler timestamps are relative to the signed clip start.
                // Clamp only events before project zero; subtract the pre-roll
                // from later timestamps so the rest of the song stays aligned.
                if (clip.position_dctpq < 0) {
                    for (auto& tick : content.ump_tick_timestamps)
                        tick = static_cast<uint64_t>(std::max<int64_t>(0, absoluteTick(tick)));
                    result.diagnostics.push_back(std::format(
                        "MML track {} starts {} ticks before project start; early events were placed at tick 0.",
                        track_key, -clip.position_dctpq));
                }
                auto separated = uapmd::MidiClipReader::separateMasterTrackEvents(std::move(content));
                const bool has_channel_events = std::ranges::any_of(clip.smf2clip, [](const auto& packet) {
                    return packet.getMessageType() == umppi::MessageType::MIDI1 ||
                           packet.getMessageType() == umppi::MessageType::MIDI2;
                });
                if (separated.hasMusicalClip() && has_channel_events) {
                    auto name = separated.musicalClip.name.value_or(std::string{});
                    result.clips.push_back({track_key + "/" + std::to_string(index),
                        name.empty() ? "MML " + track_key : name, std::max<int64_t>(0, clip.position_dctpq), false,
                        std::move(separated.musicalClip)});
                }
            }
        }
        if (!tempos.empty() || !signatures.empty()) {
            CompiledClip master;
            master.key = "master";
            master.name = "MML tempo and time signatures";
            master.master = true;
            master.content.tick_resolution = resolution;
            master.content.has_explicit_tempo_changes = !tempos.empty();
            master.content.has_explicit_time_signature_changes = !signatures.empty();
            // Retain the last pre-roll setting at zero, followed by the
            // original positive-time curve. Distinct pre-roll changes must
            // not become artificial conflicts when collapsed to project zero.
            for (auto [tick, bpm] : tempos) {
                const auto position = static_cast<uint64_t>(std::max<int64_t>(0, tick));
                if (!master.content.tempo_changes.empty() && master.content.tempo_changes.back().tickPosition == position)
                    master.content.tempo_changes.back().bpm = bpm;
                else
                    master.content.tempo_changes.push_back({position, bpm});
            }
            for (auto [tick, signature] : signatures) {
                signature.tickPosition = static_cast<uint64_t>(std::max<int64_t>(0, tick));
                if (!master.content.time_signature_changes.empty() &&
                    master.content.time_signature_changes.back().tickPosition == signature.tickPosition)
                    master.content.time_signature_changes.back() = signature;
                else
                    master.content.time_signature_changes.push_back(signature);
            }
            if (!master.content.tempo_changes.empty() && master.content.tempo_changes.front().tickPosition == 0)
                master.content.tempo = master.content.tempo_changes.front().bpm;
            result.clips.insert(result.clips.begin(), std::move(master));
        }
        result.success = true;
    } catch (const std::exception& error) {
        result.diagnostics.push_back(error.what());
    }
    return result;
}

}
