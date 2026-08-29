#include "StepSequencerEditor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include <imgui.h>
#include <umppi/umppi.hpp>

namespace uapmd_app_gui {

constexpr int StepSequencerEditor::kDivisions[kDivisionCount];

void StepSequencerEditor::showClip(int32_t trackIndex, int32_t clipId,
                                   const std::string& clipName,
                                   std::shared_ptr<ClipPreview> preview) {
    state_.trackIndex = trackIndex;
    state_.clipId = clipId;
    state_.clipName = clipName;
    state_.preview = std::move(preview);
    state_.visible = true;
    resetFromPreview();
}

void StepSequencerEditor::resetFromPreview() {
    state_.tickResolution = state_.preview && state_.preview->rawMidiData
        ? std::max(1u, state_.preview->rawMidiData->tickResolution) : 480u;
    state_.steps.assign(static_cast<size_t>(std::max(1, state_.patternSteps)), Step{});
    state_.dirty = false;
    state_.status.clear();

    if (!state_.preview || !state_.preview->rawMidiData)
        return;

    const auto& raw = *state_.preview->rawMidiData;
    for (const auto& note : state_.preview->midiNotes) {
        if (note.note != state_.note || note.noteOnWordIdx >= raw.tickTimestamps.size())
            continue;
        const uint64_t startTick = raw.tickTimestamps[note.noteOnWordIdx];
        const uint64_t offTick = note.noteOffWordIdx < raw.tickTimestamps.size()
            ? raw.tickTimestamps[note.noteOffWordIdx] : startTick + stepTicks();
        const size_t index = static_cast<size_t>(std::llround(
            static_cast<double>(startTick) / static_cast<double>(stepTicks())));
        if (index >= state_.steps.size())
            continue;
        auto& step = state_.steps[index];
        step.active = true;
        step.velocity = note.velocity;
        step.gate = std::clamp(static_cast<float>(offTick - startTick) /
                               static_cast<float>(stepTicks()), 0.05f, 1.0f);
        if (note.noteOnWordIdx < raw.umpEvents.size())
            state_.group = static_cast<uint8_t>((raw.umpEvents[note.noteOnWordIdx] >> 24) & 0xFu);
        state_.channel = note.channel;
    }
}

uint64_t StepSequencerEditor::stepTicks() const {
    const auto division = kDivisions[std::clamp(state_.divisionIndex, 0, kDivisionCount - 1)];
    return std::max<uint64_t>(1, static_cast<uint64_t>(state_.tickResolution) /
                                 static_cast<uint64_t>(division));
}

uint64_t StepSequencerEditor::patternTicks() const {
    return stepTicks() * static_cast<uint64_t>(std::max(1, state_.patternSteps));
}

void StepSequencerEditor::render(const RenderContext& context) {
    if (state_.visible)
        renderWindow(context);
}

void StepSequencerEditor::renderWindow(const RenderContext& context) {
    bool open = state_.visible;
    const auto title = std::format("Step Sequencer - {}###StepSequencer{}_{}",
                                   state_.clipName, state_.trackIndex, state_.clipId);
    ImGui::SetNextWindowSize(ImVec2(760.0f * context.uiScale, 430.0f * context.uiScale),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title.c_str(), &open)) {
        ImGui::Text("Pattern: %d steps at 1/%d", state_.patternSteps,
                    kDivisions[std::clamp(state_.divisionIndex, 0, kDivisionCount - 1)]);
        ImGui::SameLine();
        ImGui::TextDisabled("Loop repetitions: %d", state_.repetitions);

        bool changed = false;
        int note = state_.note;
        const bool noteChanged = ImGui::SliderInt("Note", &note, 0, 127);
        if (noteChanged) {
            state_.note = static_cast<uint8_t>(note);
            resetFromPreview();
        }
        ImGui::SameLine();
        changed |= ImGui::SliderInt("Steps", &state_.patternSteps, 1, 128);
        ImGui::SameLine();
        changed |= ImGui::SliderInt("Repetitions", &state_.repetitions, 1, 64);

        const char* divisionLabels[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
        if (ImGui::Combo("Division", &state_.divisionIndex, divisionLabels, kDivisionCount))
            changed = true;
        if (changed && !noteChanged) {
            const auto oldSteps = std::move(state_.steps);
            state_.steps.assign(static_cast<size_t>(state_.patternSteps), Step{});
            for (size_t i = 0; i < std::min(oldSteps.size(), state_.steps.size()); ++i)
                state_.steps[i] = oldSteps[i];
            state_.dirty = true;
        }

        if (ImGui::SliderFloat("Default Velocity", &state_.defaultVelocity, 0.0f, 1.0f))
            state_.dirty = true;
        ImGui::SameLine();
        if (ImGui::SliderFloat("Default Gate", &state_.defaultGate, 0.05f, 1.0f))
            state_.dirty = true;

        ImGui::Separator();
        ImGui::Text("Pattern length: %llu ticks | Expanded length: %llu ticks",
                    static_cast<unsigned long long>(patternTicks()),
                    static_cast<unsigned long long>(patternTicks() * state_.repetitions));
        for (int i = 0; i < state_.patternSteps; ++i) {
            auto& step = state_.steps[static_cast<size_t>(i)];
            ImGui::PushID(i);
            if (ImGui::Button(step.active ? "X" : ".",
                              ImVec2(32.0f * context.uiScale, 32.0f * context.uiScale))) {
                step.active = !step.active;
                if (step.active) {
                    step.velocity = state_.defaultVelocity;
                    step.gate = state_.defaultGate;
                }
                state_.dirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Step %d | velocity %.2f | gate %.0f%%", i + 1,
                                 step.velocity, step.gate * 100.0f);
            ImGui::SameLine();
            ImGui::PopID();
            if ((i + 1) % 16 == 0)
                ImGui::NewLine();
        }
        ImGui::NewLine();
        if (ImGui::Button("Apply Changes") && state_.dirty)
            apply(context);
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
            resetFromPreview();
        if (!state_.status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", state_.status.c_str());
        }
    }
    ImGui::End();
    state_.visible = open;
}

bool StepSequencerEditor::apply(const RenderContext& context) {
    if (!context.applyEdits || !state_.preview || !state_.preview->rawMidiData)
        return false;

    const auto& raw = *state_.preview->rawMidiData;
    struct TimedWords { uint64_t tick{}; std::vector<uapmd_ump_t> words; int order{}; };
    std::vector<TimedWords> events;
    for (size_t i = 0; i < raw.umpEvents.size();) {
        umppi::Ump ump(raw.umpEvents[i]);
        const size_t wordCount = static_cast<size_t>(std::max(1, ump.getSizeInInts()));
        const size_t end = std::min(raw.umpEvents.size(), i + wordCount);
        const auto status = ump.getStatusCode();
        const bool isNote = (ump.getMessageType() == umppi::MessageType::MIDI1 ||
                             ump.getMessageType() == umppi::MessageType::MIDI2) &&
            (status == umppi::MidiChannelStatus::NOTE_ON ||
             status == umppi::MidiChannelStatus::NOTE_OFF);
        bool ownsNote = false;
        if (isNote) {
            const auto eventNote = ump.getMessageType() == umppi::MessageType::MIDI1
                ? ump.getMidi1Note() : ump.getMidi2Note();
            ownsNote = eventNote == state_.note &&
                       ump.getGroup() == state_.group &&
                       ump.getChannelInGroup() == state_.channel;
        }
        if (!ownsNote)
            events.push_back({i < raw.tickTimestamps.size() ? raw.tickTimestamps[i] : 0,
                              {raw.umpEvents.begin() + static_cast<std::ptrdiff_t>(i),
                               raw.umpEvents.begin() + static_cast<std::ptrdiff_t>(end)}, 0});
        i = end;
    }

    const auto loopTicks = patternTicks();
    for (int repetition = 0; repetition < state_.repetitions; ++repetition) {
        const auto baseTick = loopTicks * static_cast<uint64_t>(repetition);
        for (size_t i = 0; i < state_.steps.size(); ++i) {
            const auto& step = state_.steps[i];
            if (!step.active)
                continue;
            const auto onTick = baseTick + static_cast<uint64_t>(i) * stepTicks();
            const auto offTick = onTick + std::max<uint64_t>(1,
                static_cast<uint64_t>(std::llround(step.gate * stepTicks())));
            const auto on = umppi::UmpFactory::midi2NoteOn(
                state_.group, state_.channel, state_.note, 0,
                static_cast<uint16_t>(std::llround(step.velocity * 65535.0f)), 0);
            const auto off = umppi::UmpFactory::midi2NoteOff(
                state_.group, state_.channel, state_.note, 0, 0, 0);
            events.push_back({onTick, {static_cast<uapmd_ump_t>(on >> 32),
                                       static_cast<uapmd_ump_t>(on)}, 1});
            events.push_back({offTick, {static_cast<uapmd_ump_t>(off >> 32),
                                        static_cast<uapmd_ump_t>(off)}, -1});
        }
    }

    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.order < b.order;
    });
    std::vector<uapmd_ump_t> newEvents;
    std::vector<uint64_t> newTicks;
    for (const auto& event : events) {
        newEvents.insert(newEvents.end(), event.words.begin(), event.words.end());
        newTicks.insert(newTicks.end(), event.words.size(), event.tick);
    }
    std::string error;
    if (!context.applyEdits(state_.trackIndex, state_.clipId, std::move(newEvents),
                            std::move(newTicks), error)) {
        state_.status = error.empty() ? "Apply failed" : error;
        return false;
    }
    state_.dirty = false;
    state_.status = "Applied";
    if (context.reloadPreview) {
        if (auto preview = context.reloadPreview(state_.trackIndex, state_.clipId)) {
            state_.preview = std::move(preview);
            resetFromPreview();
            state_.status = "Applied";
        }
    }
    return true;
}

} // namespace uapmd_app_gui
