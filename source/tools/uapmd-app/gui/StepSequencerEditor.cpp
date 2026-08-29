#include "StepSequencerEditor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include <imgui.h>
#include <umppi/umppi.hpp>

namespace uapmd_app_gui {

constexpr int StepSequencerEditor::kDivisions[kDivisionCount];

namespace {

const char* gmDrumName(uint8_t note) {
    switch (note) {
        case 35: return "Acoustic Bass Drum";
        case 36: return "Bass Drum 1";
        case 37: return "Side Stick";
        case 38: return "Acoustic Snare";
        case 39: return "Hand Clap";
        case 40: return "Electric Snare";
        case 41: return "Low Floor Tom";
        case 42: return "Closed Hi-Hat";
        case 43: return "High Floor Tom";
        case 44: return "Pedal Hi-Hat";
        case 45: return "Low Tom";
        case 46: return "Open Hi-Hat";
        case 47: return "Low-Mid Tom";
        case 48: return "Hi-Mid Tom";
        case 49: return "Crash Cymbal 1";
        case 50: return "High Tom";
        case 51: return "Ride Cymbal 1";
        case 52: return "Chinese Cymbal";
        case 53: return "Ride Bell";
        case 54: return "Tambourine";
        case 55: return "Splash Cymbal";
        case 56: return "Cowbell";
        case 57: return "Crash Cymbal 2";
        case 58: return "Vibraslap";
        case 59: return "Ride Cymbal 2";
        case 60: return "Hi Bongo";
        case 61: return "Low Bongo";
        case 62: return "Mute Hi Conga";
        case 63: return "Open Hi Conga";
        case 64: return "Low Conga";
        case 65: return "High Timbale";
        case 66: return "Low Timbale";
        case 67: return "High Agogo";
        case 68: return "Low Agogo";
        case 69: return "Cabasa";
        case 70: return "Maracas";
        case 71: return "Short Whistle";
        case 72: return "Long Whistle";
        case 73: return "Short Guiro";
        case 74: return "Long Guiro";
        case 75: return "Claves";
        case 76: return "Hi Wood Block";
        case 77: return "Low Wood Block";
        case 78: return "Mute Cuica";
        case 79: return "Open Cuica";
        case 80: return "Mute Triangle";
        case 81: return "Open Triangle";
        default: return nullptr;
    }
}

std::string noteLabel(uint8_t note, bool includeDrumName) {
    static constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F",
                                             "F#", "G", "G#", "A", "A#", "B"};
    auto label = std::format("{}{} ({})", names[note % 12], static_cast<int>(note / 12) - 1,
                             static_cast<int>(note));
    if (includeDrumName)
        return std::format("{}  {}", label, gmDrumName(note));
    return label;
}

} // namespace

void StepSequencerEditor::showClip(int32_t trackIndex, int32_t clipId,
                                   const std::string& clipName,
                                   std::shared_ptr<ClipPreview> preview) {
    state_.trackIndex = trackIndex;
    state_.clipId = clipId;
    state_.clipName = clipName;
    state_.preview = std::move(preview);
    state_.visible = true;
    resetFromPreview(true);
}

void StepSequencerEditor::resetFromPreview(bool focusDrumRoot) {
    state_.tickResolution = state_.preview && state_.preview->rawMidiData
        ? std::max(1u, state_.preview->rawMidiData->tickResolution) : 480u;
    rebuildLanes();
    state_.focusDrumRoot = focusDrumRoot && state_.noteSet == NoteSet::GmDrums;
    state_.dirty = false;
    state_.status.clear();

    if (!state_.preview || !state_.preview->rawMidiData)
        return;

    const auto& raw = *state_.preview->rawMidiData;
    if (!state_.preview->midiNotes.empty()) {
        const auto& firstNote = state_.preview->midiNotes.front();
        state_.channel = firstNote.channel;
        if (firstNote.noteOnWordIdx < raw.umpEvents.size())
            state_.group = static_cast<uint8_t>((raw.umpEvents[firstNote.noteOnWordIdx] >> 24) & 0xFu);
    }
    for (const auto& note : state_.preview->midiNotes) {
        if (note.noteOnWordIdx >= raw.tickTimestamps.size() || note.channel != state_.channel)
            continue;
        if (note.noteOnWordIdx >= raw.umpEvents.size() ||
            static_cast<uint8_t>((raw.umpEvents[note.noteOnWordIdx] >> 24) & 0xFu) != state_.group)
            continue;
        auto lane = std::find_if(state_.lanes.begin(), state_.lanes.end(), [&note](const auto& candidate) {
            return candidate.note == note.note;
        });
        if (lane == state_.lanes.end())
            continue;
        const uint64_t startTick = raw.tickTimestamps[note.noteOnWordIdx];
        const uint64_t offTick = note.noteOffWordIdx < raw.tickTimestamps.size()
            ? raw.tickTimestamps[note.noteOffWordIdx] : startTick + stepTicks();
        const size_t index = static_cast<size_t>(std::llround(
            static_cast<double>(startTick) / static_cast<double>(stepTicks())));
        if (index >= lane->steps.size())
            continue;
        auto& step = lane->steps[index];
        step.active = true;
        step.velocity = note.velocity;
        step.gate = std::clamp(static_cast<float>(offTick - startTick) /
                               static_cast<float>(stepTicks()), 0.05f, 1.0f);
    }
}

void StepSequencerEditor::rebuildLanes() {
    state_.lanes.clear();
    const int first = state_.noteSet == NoteSet::GmDrums ? 81 : 127;
    const int last = state_.noteSet == NoteSet::GmDrums ? 35 : 0;
    for (int note = first; note >= last; --note)
        state_.lanes.push_back({static_cast<uint8_t>(note),
                                std::vector<Step>(static_cast<size_t>(std::max(1, state_.patternSteps)))});
}

void StepSequencerEditor::resizeLanes(int patternSteps) {
    for (auto& lane : state_.lanes)
        lane.steps.resize(static_cast<size_t>(std::max(1, patternSteps)));
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
    ImGui::SetNextWindowSize(ImVec2(900.0f * context.uiScale, 620.0f * context.uiScale),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title.c_str(), &open)) {
        ImGui::Text("Pattern: %d steps at 1/%d", state_.patternSteps,
                    kDivisions[std::clamp(state_.divisionIndex, 0, kDivisionCount - 1)]);
        ImGui::SameLine();
        ImGui::TextDisabled("Loop repetitions: %d", state_.repetitions);

        bool changed = false;
        bool editedThisFrame = false;
        int noteSet = state_.noteSet == NoteSet::GmDrums ? 0 : 1;
        const char* noteSetLabels[] = {"GM Drums", "All Notes"};
        ImGui::SetNextItemWidth(110.0f * context.uiScale);
        if (ImGui::Combo("Note set", &noteSet, noteSetLabels, IM_ARRAYSIZE(noteSetLabels))) {
            state_.noteSet = noteSet == 0 ? NoteSet::GmDrums : NoteSet::AllNotes;
            resetFromPreview(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75.0f * context.uiScale);
        changed |= ImGui::SliderInt("Steps", &state_.patternSteps, 1, 128);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75.0f * context.uiScale);
        changed |= ImGui::SliderInt("Repetitions", &state_.repetitions, 1, 64);

        const char* divisionLabels[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
        ImGui::SetNextItemWidth(80.0f * context.uiScale);
        if (ImGui::Combo("Division", &state_.divisionIndex, divisionLabels, kDivisionCount))
            changed = true;
        if (changed) {
            resizeLanes(state_.patternSteps);
            state_.dirty = true;
            editedThisFrame = true;
        }

        ImGui::SetNextItemWidth(90.0f * context.uiScale);
        if (ImGui::SliderFloat("Default Velocity", &state_.defaultVelocity, 0.0f, 1.0f)) {
            state_.dirty = true;
            editedThisFrame = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f * context.uiScale);
        if (ImGui::SliderFloat("Default Gate", &state_.defaultGate, 0.05f, 1.0f)) {
            state_.dirty = true;
            editedThisFrame = true;
        }

        ImGui::Separator();
        ImGui::Text("Pattern length: %llu ticks | Expanded length: %llu ticks",
                    static_cast<unsigned long long>(patternTicks()),
                    static_cast<unsigned long long>(patternTicks() * state_.repetitions));
        ImGui::BeginChild("##StepSequencerLanes", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f),
                          true, ImGuiWindowFlags_HorizontalScrollbar);
        const float labelWidth = 190.0f * context.uiScale;
        const float buttonSize = 26.0f * context.uiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * context.uiScale, 2.0f * context.uiScale));
        if (state_.focusDrumRoot) {
            // MIDI 36 (C2) is the usual kick-drum root for a GM pattern.
            ImGui::SetScrollY(static_cast<float>(81 - 36) *
                              (buttonSize + ImGui::GetStyle().ItemSpacing.y));
            state_.focusDrumRoot = false;
        }
        ImGui::TextUnformatted("Note");
        ImGui::SameLine(labelWidth);
        for (int i = 0; i < state_.patternSteps; ++i) {
            ImGui::Text("%d", i + 1);
            if (i + 1 < state_.patternSteps)
                ImGui::SameLine(labelWidth + (i + 1) * (buttonSize + ImGui::GetStyle().ItemSpacing.x));
        }
        ImGui::Separator();
        for (auto& lane : state_.lanes) {
            ImGui::PushID(lane.note);
            const auto label = noteLabel(lane.note, state_.noteSet == NoteSet::GmDrums);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine(labelWidth);
            for (int i = 0; i < state_.patternSteps; ++i) {
                auto& step = lane.steps[static_cast<size_t>(i)];
                ImGui::PushID(i);
                const auto baseColor = step.active
                    ? ImVec4(0.18f, 0.64f, 0.42f, 1.0f)
                    : ImVec4(0.20f, 0.22f, 0.25f, 1.0f);
                const auto hoverColor = step.active
                    ? ImVec4(0.25f, 0.76f, 0.51f, 1.0f)
                    : ImVec4(0.29f, 0.32f, 0.36f, 1.0f);
                const auto heldColor = step.active
                    ? ImVec4(0.13f, 0.50f, 0.32f, 1.0f)
                    : ImVec4(0.14f, 0.16f, 0.18f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, baseColor);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, heldColor);
                if (ImGui::Button("##step", ImVec2(buttonSize, buttonSize))) {
                    step.active = !step.active;
                    if (step.active) {
                        step.velocity = state_.defaultVelocity;
                        step.gate = state_.defaultGate;
                    }
                    state_.dirty = true;
                    editedThisFrame = true;
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s | Step %d | velocity %.2f | gate %.0f%%", label.c_str(),
                                      i + 1, step.velocity, step.gate * 100.0f);
                ImGui::PopID();
                if (i + 1 < state_.patternSteps)
                    ImGui::SameLine();
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (editedThisFrame && state_.dirty)
            apply(context);
        ImGui::TextDisabled("Changes apply immediately");
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
            ownsNote = ump.getGroup() == state_.group &&
                       ump.getChannelInGroup() == state_.channel &&
                       std::any_of(state_.lanes.begin(), state_.lanes.end(), [eventNote](const auto& lane) {
                           return lane.note == eventNote;
                       });
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
        for (const auto& lane : state_.lanes) {
            for (size_t i = 0; i < lane.steps.size(); ++i) {
                const auto& step = lane.steps[i];
                if (!step.active)
                    continue;
                const auto onTick = baseTick + static_cast<uint64_t>(i) * stepTicks();
                const auto offTick = onTick + std::max<uint64_t>(1,
                    static_cast<uint64_t>(std::llround(step.gate * stepTicks())));
                const auto on = umppi::UmpFactory::midi2NoteOn(
                    state_.group, state_.channel, lane.note, 0,
                    static_cast<uint16_t>(std::llround(step.velocity * 65535.0f)), 0);
                const auto off = umppi::UmpFactory::midi2NoteOff(
                    state_.group, state_.channel, lane.note, 0, 0, 0);
                events.push_back({onTick, {static_cast<uapmd_ump_t>(on >> 32),
                                           static_cast<uapmd_ump_t>(on)}, 1});
                events.push_back({offTick, {static_cast<uapmd_ump_t>(off >> 32),
                                            static_cast<uapmd_ump_t>(off)}, -1});
            }
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
