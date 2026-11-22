#include "MixerWindow.hpp"
#include "TrackMixActions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <unordered_set>
#include <utility>

#include <uapmd-app-model/uapmd-app-model.hpp>

namespace uapmd_app_gui {

namespace {

constexpr float kMinDb = -60.0f;
constexpr float kMaxGainDb = 18.0618f;
constexpr float kUnityPosition = 0.7f;
constexpr size_t kMaxStripChannels = 8;
constexpr float kStripWidth = 120.0f;

ImU32 rgb(uint32_t color) {
    return IM_COL32(color >> 16, (color >> 8) & 0xff, color & 0xff, 255);
}

bool darkMixer() {
    const auto background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return background.x + background.y + background.z < 1.5f;
}

ImU32 overloadColor() {
    return rgb(darkMixer() ? 0xf29a9e : 0x9e2d36);
}

class MixerStyle {
    struct Color {
        ImGuiCol slot;
        uint32_t dark;
        uint32_t light;
    };

    static constexpr std::array colors_{
        Color{ImGuiCol_WindowBg,         0x20252c, 0xf8f9fb},
        Color{ImGuiCol_ChildBg,          0x20252c, 0xf8f9fb},
        Color{ImGuiCol_PopupBg,          0x262d36, 0xeff3f7},
        Color{ImGuiCol_Text,             0xedf3f9, 0x172839},
        Color{ImGuiCol_TextDisabled,     0xafbed0, 0x465a70},
        Color{ImGuiCol_Border,           0x3e4b5a, 0xcbd4de},
        Color{ImGuiCol_Button,           0x28323d, 0xf8f9fb},
        Color{ImGuiCol_ButtonHovered,    0x2d4054, 0xe2edf8},
        Color{ImGuiCol_ButtonActive,     0x38516b, 0xcbdff2},
    };

public:
    MixerStyle() {
        const bool dark = darkMixer();
        for (const auto& color : colors_)
            ImGui::PushStyleColor(color.slot, rgb(dark ? color.dark : color.light));
    }

    ~MixerStyle() {
        ImGui::PopStyleColor(static_cast<int>(colors_.size()));
    }
};

float peakDb(float peak) {
    return peak > 0.0f ? 20.0f * std::log10(peak) : kMinDb;
}

float meterPosition(float db) {
    return std::clamp((db - kMinDb) / -kMinDb, 0.0f, 1.0f);
}

float gainPosition(double gain) {
    const auto db = std::clamp(peakDb(static_cast<float>(gain)), kMinDb, kMaxGainDb);
    if (db <= 0.0f) {
        const auto normalized = meterPosition(db);
        return kUnityPosition * normalized * normalized;
    }
    return kUnityPosition + (1.0f - kUnityPosition) * db / kMaxGainDb;
}

double gainAtPosition(float position) {
    if (position <= 0.0f)
        return 0.0;
    const auto db = position <= kUnityPosition
        ? kMinDb * (1.0f - std::sqrt(position / kUnityPosition))
        : kMaxGainDb * (position - kUnityPosition) / (1.0f - kUnityPosition);
    return std::min(8.0, std::pow(10.0, db / 20.0));
}

std::string channelLabel(size_t channel, size_t count) {
    if (count == 2)
        return channel == 0 ? "L" : "R";
    return std::to_string(channel + 1);
}

void drawMeter(ImDrawList& draw, ImVec2 pos, ImVec2 size, float db, float heldPeak, bool vertical) {
    const ImVec2 end{pos.x + size.x, pos.y + size.y};
    const bool dark = darkMixer();
    draw.AddRectFilled(pos, end, rgb(dark ? 0x141a21 : 0xd3dce6), 2.0f);
    const auto fill = meterPosition(db);
    const auto signal = rgb(dark ? 0x76cfa0 : 0x247858);
    if (fill > 0.0f) {
        if (vertical)
            draw.AddRectFilled(ImVec2(pos.x, end.y - fill * size.y), end, signal, 2.0f);
        else
            draw.AddRectFilled(pos, ImVec2(pos.x + fill * size.x, end.y), signal, 2.0f);
    }
    if (heldPeak > 0.0f) {
        const auto hold = meterPosition(peakDb(heldPeak));
        const ImU32 color = heldPeak >= 1.0f ? overloadColor() : ImGui::GetColorU32(ImGuiCol_Text);
        if (vertical) {
            const auto y = std::clamp(end.y - hold * size.y, pos.y + 1.0f, end.y - 1.0f);
            draw.AddLine(ImVec2(pos.x, y), ImVec2(end.x, y), color, 2.0f);
        } else {
            const auto x = std::clamp(pos.x + hold * size.x, pos.x + 1.0f, end.x - 1.0f);
            draw.AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), color, 2.0f);
        }
    }
}

float buttonWidth(const char* label) {
    return std::max(ImGui::GetFrameHeight(),
        ImGui::CalcTextSize(label, nullptr, true).x + 2.0f * ImGui::GetStyle().FramePadding.x);
}

void centerItem(float width) {
    const auto x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + std::max(0.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
}

bool stripButton(const char* label, ImVec2 size, bool selected, ImU32 color) {
    if (selected) {
        const auto tint = ImGui::ColorConvertU32ToFloat4(color);
        auto background = tint;
        background.w = 0.18f;
        ImGui::PushStyleColor(ImGuiCol_Button, background);
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        ImGui::PushStyleColor(ImGuiCol_Border, tint);
    }
    const bool pressed = ImGui::Button(label,
        ImVec2(std::max(size.x, buttonWidth(label)), std::max(size.y, ImGui::GetFrameHeight())));
    if (selected)
        ImGui::PopStyleColor(3);
    return pressed;
}

std::string outputRoute(uapmd::SequencerEngine& engine, int32_t trackIndex, size_t bus) {
    if (trackIndex == uapmd::kMasterTrackIndex)
        return "Device output";
    const auto target = engine.trackOutputBusRoutingTarget(trackIndex, static_cast<uint32_t>(bus));
    const char* folded = target.folded ? " (folded)" : "";
    switch (target.type) {
    case uapmd_graph::TrackOutputRoutingTargetType::MASTER_INPUT_BUS:
        return std::format("Master In {}{}", target.bus_index + 1, folded);
    case uapmd_graph::TrackOutputRoutingTargetType::MAIN_MIX_BUS:
        return std::format("Main Mix {}{}", target.bus_index + 1, folded);
    default:
        return "Unrouted";
    }
}

}

struct MixerWindow::Strip {
    int32_t index;
    uapmd::SequencerTrack* track;
    std::string key;
    std::string title;
    uapmd::TrackProperties properties;
    TrackMeters* meters{};
};

void MixerWindow::setCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void MixerWindow::toggle() {
    visible_ = !visible_;
    request_focus_ = visible_;
}

void MixerWindow::showTrack(int32_t trackIndex) {
    visible_ = true;
    request_focus_ = true;
    focus_track_ = trackIndex;
}

void MixerWindow::resetMeterDisplay() {
    meters_.clear();
    output_track_.clear();
    focus_track_.reset();
    reveal_output_.reset();
}

void MixerWindow::updateGesture(uapmd::ProjectCommandManager& history, const char* label) {
    const auto item = ImGui::GetItemID();
    if (ImGui::IsItemActivated()) {
        finishGesture(&history);
        if (history.beginGesture(label).succeeded()) {
            gesture_id_ = item;
            gesture_history_ = &history;
        }
    }
    if (gesture_id_ == item && gesture_history_ == &history)
        gesture_active_ = ImGui::IsItemActive();
}

void MixerWindow::finishGesture(uapmd::ProjectCommandManager* history) {
    const bool ownsGesture = gesture_id_ && gesture_history_ == history;
    gesture_id_ = 0;
    gesture_history_ = nullptr;
    if (ownsGesture)
        history->endGesture();
}

void MixerWindow::readMeters(Strip& strip, float releaseDb) {
    auto& display = meters_[strip.key];
    strip.meters = &display;
    display.buses.resize(strip.track->outputMeterBusCount());
    for (size_t bus = 0; bus < display.buses.size(); ++bus) {
        auto& channels = display.buses[bus];
        channels.resize(strip.track->outputMeterChannelCount(bus));
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            auto& state = channels[channel];
            const auto peak = strip.track->consumeOutputPeak(bus, channel);
            state.level_db = std::max({kMinDb, peakDb(peak), state.level_db - releaseDb});
            state.held_peak = std::max(state.held_peak, peak);
        }
    }
    if (display.selected_bus >= display.buses.size() || display.buses[display.selected_bus].empty()) {
        const auto first = std::ranges::find_if(display.buses, [](const auto& bus) { return !bus.empty(); });
        display.selected_bus = static_cast<size_t>(first - display.buses.begin());
    }
}

void MixerWindow::renderStrip(uapmd::SequencerEngine& engine, Strip& strip, float meterHeight, float uiScale) {
    auto& track = *strip.track;
    auto& display = *strip.meters;
    auto& commands = engine.commands();
    const bool master = strip.index == uapmd::kMasterTrackIndex;
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float stripStart = ImGui::GetCursorPosX();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool showingOutputs = output_track_ == strip.key;
    const auto selectOutputs = [&] {
        output_track_ = strip.key;
        reveal_output_ = display.selected_bus;
    };

    if (strip.properties.color) {
        const auto color = *strip.properties.color;
        const auto pos = ImGui::GetCursorScreenPos();
        const auto y = pos.y + ImGui::GetStyle().FramePadding.y;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - 5.0f * uiScale, y),
            ImVec2(pos.x - 2.0f * uiScale, y + ImGui::GetTextLineHeight()), rgb(color));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (stripButton(strip.title.c_str(), ImVec2(available, 0.0f), showingOutputs, ImGui::GetColorU32(ImGuiCol_HeaderActive)))
        selectOutputs();
    ImGui::PopStyleVar();
    ImGui::SetItemTooltip("Show output meters for %s", strip.title.c_str());
    if (master)
        ImGui::TextDisabled("Device output");
    else if (!track.orderedInstanceIds().empty()) {
        const auto* plugin = engine.getPluginInstance(track.orderedInstanceIds().front());
        const std::string pluginName = plugin ? plugin->displayName() : std::string{};
        ImGui::TextDisabled("%s", pluginName.c_str());
        if (!pluginName.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", pluginName.c_str());
    } else {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }

    const auto pannerBus = static_cast<uint32_t>(display.selected_bus);
    const auto stereoBalance = master ? std::nullopt : track.stereoBalance(pannerBus);
    if (stereoBalance) {
        float balance = *stereoBalance * 100.0f;
        const int rounded = static_cast<int>(std::round(balance));
        const auto value = rounded == 0 ? std::string("0###BalanceCenter")
            : std::format("{}{}###BalanceCenter", std::abs(rounded), rounded < 0 ? "L" : "R");
        const auto readoutWidth = std::min(available, std::max(buttonWidth("100L"), buttonWidth("100R")));
        const auto labelRoom = available - spacing - readoutWidth;
        const char* balanceLabel = ImGui::CalcTextSize("Balance").x <= labelRoom ? "Balance" : "Bal.";
        if (ImGui::CalcTextSize(balanceLabel).x <= labelRoom) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", balanceLabel);
            ImGui::SameLine(stripStart + available - readoutWidth);
        } else
            ImGui::SetCursorPosX(stripStart + available - readoutWidth);
        if (ImGui::Button(value.c_str(), ImVec2(readoutWidth, 0)))
            commands.setTrackOutputPanners(strip.index, track.outputPannersWithStereoBalance(pannerBus, 0.0f));
        ImGui::SetItemTooltip("Center balance");
        ImGui::SetNextItemWidth(available);
        const bool changed = ImGui::SliderFloat("##Balance", &balance, -100.0f, 100.0f, "", ImGuiSliderFlags_AlwaysClamp);
        updateGesture(commands.history(), "Change track balance");
        if (changed)
            commands.setTrackOutputPanners(strip.index, track.outputPannersWithStereoBalance(pannerBus, balance / 100.0f));
        ImGui::SetItemTooltip("Stereo balance for bus %u", pannerBus + 1);
    } else {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
    }

    const float faderWidth = 24.0f * uiScale;
    const auto* channels = display.selected_bus < display.buses.size() ? &display.buses[display.selected_bus] : nullptr;
    const bool aggregate = channels && channels->size() > kMaxStripChannels;
    const size_t lanes = channels ? (aggregate ? 1 : channels->size()) : 0;
    const float laneGap = 3.0f * uiScale;
    const float controlGap = 9.0f * uiScale;
    const float laneWidth = lanes == 0 ? 0.0f : std::clamp(
        (available - faderWidth - controlGap - laneGap * (lanes - 1)) / lanes, 2.0f, 9.0f * uiScale);
    const float meterWidth = lanes == 0 ? 0.0f : lanes * (laneWidth + laneGap) - laneGap;
    centerItem(faderWidth + (lanes == 0 ? 0.0f : controlGap + meterWidth));
    const auto faderPos = ImGui::GetCursorScreenPos();
    const float rail = faderPos.x + faderWidth * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(rail - 1.5f * uiScale, faderPos.y),
        ImVec2(rail + 1.5f * uiScale, faderPos.y + meterHeight), ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    const double gain = track.trackGain();
    float position = gainPosition(gain);
    const bool gainChanged = ImGui::VSliderFloat(
        "##Gain", ImVec2(faderWidth, meterHeight), &position, 0.0f, 1.0f, "");
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    updateGesture(commands.history(), "Change track gain");
    if (gainChanged && commands.setTrackGain(strip.index, gainAtPosition(position)))
        engine.markTrackDirty(strip.index, true);
    ImGui::SetItemTooltip("Track gain");

    if (lanes != 0) {
        ImGui::SameLine(0.0f, controlGap);
        const auto pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##OutputMeter", ImVec2(meterWidth, meterHeight + ImGui::GetTextLineHeight()),
            ImGuiButtonFlags_EnableNav))
            selectOutputs();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetItemTooltip("Show bus %zu (%zu channels) below", display.selected_bus + 1, channels->size());
        auto& draw = *ImGui::GetWindowDrawList();
        for (size_t channel = 0; channel < lanes; ++channel) {
            const auto lane = ImVec2(pos.x + channel * (laneWidth + laneGap), pos.y);
            auto meter = (*channels)[channel];
            if (aggregate)
                for (const auto& item : *channels) {
                    meter.level_db = std::max(meter.level_db, item.level_db);
                    meter.held_peak = std::max(meter.held_peak, item.held_peak);
                }
            drawMeter(draw, lane, ImVec2(laneWidth, meterHeight), meter.level_db, meter.held_peak, true);
            if (lanes <= 2 && !aggregate) {
                const auto label = channelLabel(channel, channels->size());
                const auto x = lane.x + (laneWidth - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
                draw.AddText(ImVec2(x, lane.y + meterHeight + 1.0f),
                    ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled), label.c_str());
            }
        }
    } else {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }

    const auto gainLabel = gain > 0.0 ? std::format("{:.1f} dB", peakDb(static_cast<float>(gain))) : "-inf dB";
    centerItem(ImGui::CalcTextSize(gainLabel.c_str()).x);
    ImGui::TextUnformatted(gainLabel.c_str());

    if (!master) {
        const float width = std::max(buttonWidth("M"), buttonWidth("S"));
        centerItem(2.0f * width + spacing);
        if (stripButton("M", ImVec2(width, 0.0f), track.muted(), overloadColor()))
            commands.setTrackMuted(strip.index, !track.muted());
        ImGui::SetItemTooltip("%s", track.muted() ? "Unmute track" : "Mute track");
        ImGui::SameLine();
        if (stripButton("S", ImVec2(width, 0.0f), track.solo(), rgb(darkMixer() ? 0xf4c46c : 0x805400)))
            commands.setTrackSolo(strip.index, !track.solo());
        ImGui::SetItemTooltip("%s", track.solo() ? "Unsolo track" : "Solo track");
    } else {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
    }

    if (!channels)
        return;
    const bool multipleBuses = display.buses.size() > 1;

    float largestPeak = 0.0f;
    size_t overBus = 0;
    size_t overChannel = 0;
    for (size_t bus = 0; bus < display.buses.size(); ++bus)
        for (size_t channel = 0; channel < display.buses[bus].size(); ++channel)
            if (display.buses[bus][channel].held_peak > largestPeak) {
                largestPeak = display.buses[bus][channel].held_peak;
                overBus = bus;
                overChannel = channel;
            }
    std::string busLabel;
    if (multipleBuses)
        busLabel = std::format("{}/{}###Outputs", display.selected_bus + 1, display.buses.size());
    else if (channels->size() > 2)
        busLabel = std::format("{} ch###Outputs", channels->size());

    std::string warning;
    if (largestPeak >= 1.0f) {
        const auto channel = channelLabel(overChannel, display.buses[overBus].size());
        warning = multipleBuses
            ? std::format("! {}{}###Over", overBus + 1, channel)
            : std::format("! {}###Over", channel);
    }
    if (busLabel.empty() && warning.empty())
        return;
    const auto footerWidth = (busLabel.empty() ? 0.0f : buttonWidth(busLabel.c_str())) +
        (warning.empty() ? 0.0f : buttonWidth(warning.c_str())) +
        (busLabel.empty() || warning.empty() ? 0.0f : spacing);
    centerItem(footerWidth);
    if (!busLabel.empty()) {
        if (stripButton(busLabel.c_str(), ImVec2(0, 0), showingOutputs, ImGui::GetColorU32(ImGuiCol_HeaderActive)))
            selectOutputs();
        ImGui::SetItemTooltip("Show bus %zu of %zu below", display.selected_bus + 1, display.buses.size());
    }
    if (!warning.empty()) {
        if (!busLabel.empty() && footerWidth <= available)
            ImGui::SameLine();
        if (stripButton(warning.c_str(), ImVec2(0, 0), true, overloadColor())) {
            display.selected_bus = overBus;
            selectOutputs();
        }
        ImGui::SetItemTooltip("Bus %zu channel %s held at %+.1f dBFS", overBus + 1,
            channelLabel(overChannel, display.buses[overBus].size()).c_str(), peakDb(largestPeak));
    }
}

void MixerWindow::renderOutputs(uapmd::SequencerEngine& engine, Strip& strip, float uiScale) {
    ImGui::SeparatorText(std::format("{} outputs", strip.title).c_str());
    auto& display = *strip.meters;
    const auto busCount = std::ranges::count_if(display.buses, [](const auto& bus) { return !bus.empty(); });
    if (busCount == 0) {
        ImGui::TextDisabled("No output channels");
        return;
    }
    ImGui::TextDisabled("Peak hold (dBFS)");
    const bool twoColumns = ImGui::GetContentRegionAvail().x >= 520.0f * uiScale && busCount > 1;
    if (!ImGui::BeginTable("OutputMeters", twoColumns ? 2 : 1, ImGuiTableFlags_SizingStretchSame))
        return;
    for (size_t bus = 0; bus < display.buses.size(); ++bus) {
        const auto& channels = display.buses[bus];
        if (channels.empty())
            continue;
        ImGui::PushID(static_cast<int>(bus));
        ImGui::TableNextColumn();
        if (busCount > 1) {
            const auto label = std::to_string(bus + 1);
            const auto width = std::max(26.0f * uiScale, buttonWidth(label.c_str()));
            if (stripButton(label.c_str(), ImVec2(width, 0.0f),
                display.selected_bus == bus, ImGui::GetColorU32(ImGuiCol_HeaderActive)))
                display.selected_bus = bus;
            ImGui::SetItemTooltip("Show bus %zu beside the fader", bus + 1);
            if (reveal_output_ == bus)
                ImGui::SetScrollHereY(0.25f);
            ImGui::SameLine();
        }
        const auto route = outputRoute(engine, strip.index, bus);
        ImGui::TextDisabled("%s", route.c_str());
        ImGui::SetItemTooltip("%s", route.c_str());
        const auto cellX = ImGui::GetCursorPosX();
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            const auto& meter = channels[channel];
            ImGui::TextUnformatted(channelLabel(channel, channels.size()).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(cellX + 21.0f * uiScale);
            const float height = ImGui::GetTextLineHeight();
            const float width = std::max(2.0f, ImGui::GetContentRegionAvail().x - 48.0f * uiScale);
            auto pos = ImGui::GetCursorScreenPos();
            pos.y += height * 0.22f;
            drawMeter(*ImGui::GetWindowDrawList(), pos, ImVec2(width, height * 0.56f),
                meter.level_db, meter.held_peak, false);
            ImGui::Dummy(ImVec2(width, height));
            ImGui::SameLine();
            if (meter.held_peak >= 1.0f)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(overloadColor()), "%+.1f", peakDb(meter.held_peak));
            else if (meter.held_peak > 0.0f)
                ImGui::Text("%.1f", peakDb(meter.held_peak));
            else
                ImGui::TextDisabled("-inf");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void MixerWindow::render(float uiScale) {
    auto& app = uapmd_app::AppModel::instance();
    auto* engine = app.sequencer().engine();
    if (!visible_ || !engine) {
        if (engine)
            engine->captureMixerPeaks(false);
        finishGesture(engine ? &engine->commands().history() : nullptr);
        return;
    }

    constexpr auto windowId = "Mixer";
    const MixerStyle mixerStyle;
    const auto trackList = engine->tracks();
    if (callbacks_.set_child_size)
        callbacks_.set_child_size(windowId, ImVec2(std::clamp(kStripWidth * (trackList.size() + 1) + 40.0f, 500.0f, 870.0f), 620.0f));
    const auto viewportSize = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(
        std::min(320.0f * uiScale, viewportSize.x), std::min(400.0f * uiScale, viewportSize.y)), viewportSize);
    if (std::exchange(request_focus_, false))
        ImGui::SetNextWindowFocus();
    const bool expanded = ImGui::Begin(windowId, &visible_);
    engine->captureMixerPeaks(expanded && visible_);
    if (!expanded || !visible_) {
        finishGesture(&engine->commands().history());
        ImGui::End();
        return;
    }
    if (callbacks_.update_child_size_state)
        callbacks_.update_child_size_state(windowId);

    // The window uses the shared title-bar geometry. Tighten only the horizontal
    // padding inside the body, keeping the shared frame height as the button minimum.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * uiScale, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * uiScale);

    auto& commands = engine->commands();
    gesture_active_ = false;
    const auto timelineTracks = app.getTimelineTracks();
    const auto makeStrip = [](int32_t index, uapmd::SequencerTrack& track, const uapmd::TimelineTrack* timeline) {
        const bool master = index == uapmd::kMasterTrackIndex;
        const auto title = master ? std::string("Master") : std::format("Track {}", index + 1);
        const auto key = timeline ? std::format("Track:{}", timeline->referenceId()) : title;
        return Strip{index, &track, key, title, timeline ? timeline->properties() : uapmd::TrackProperties{}};
    };
    std::vector<Strip> strips;
    strips.reserve(trackList.size() + 1);
    for (size_t index = 0; index < trackList.size(); ++index)
        if (trackList[index])
            strips.push_back(makeStrip(static_cast<int32_t>(index), *trackList[index],
                index < timelineTracks.size() ? timelineTracks[index] : nullptr));
    std::stable_sort(strips.begin(), strips.end(), [](const Strip& a, const Strip& b) {
        return uapmd::trackPresentationLess(a.properties, b.properties);
    });
    const auto regularCount = strips.size();
    if (auto* master = engine->masterTrack())
        strips.push_back(makeStrip(uapmd::kMasterTrackIndex, *master, app.getMasterTimelineTrack()));
    const bool hasMaster = strips.size() > regularCount;

    const auto releaseDb = 48.0f * ImGui::GetIO().DeltaTime;
    std::unordered_set<std::string_view> activeTracks;
    for (auto& strip : strips) {
        activeTracks.insert(strip.key);
        readMeters(strip, releaseDb);
        if (focus_track_ == strip.index)
            output_track_ = strip.key;
    }
    std::erase_if(meters_, [&](const auto& entry) { return !activeTracks.contains(entry.first); });
    if (!activeTracks.contains(output_track_))
        output_track_ = strips.empty() ? std::string{} : (hasMaster ? strips.back() : strips.front()).key;

    const bool anyMuted = std::ranges::any_of(trackList, [](auto* track) { return track && track->muted(); });
    const bool anySolo = std::ranges::any_of(trackList, [](auto* track) { return track && track->solo(); });
    const char* muteLabel = anyMuted ? "Unmute all" : "Mute all";
    const char* soloLabel = anySolo ? "Unsolo all" : "Solo all";
    ImGui::BeginDisabled(regularCount == 0);
    if (ImGui::Button(muteLabel, ImVec2(buttonWidth(muteLabel), 0.0f)))
        setAllTracks(commands, trackList, true, !anyMuted, muteLabel);
    ImGui::SameLine();
    if (ImGui::Button(soloLabel, ImVec2(buttonWidth(soloLabel), 0.0f)))
        setAllTracks(commands, trackList, false, !anySolo, soloLabel);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear peaks", ImVec2(buttonWidth("Clear peaks"), 0.0f)))
        for (auto& [key, display] : meters_)
            for (auto& bus : display.buses)
                for (auto& channel : bus)
                    channel.held_peak = 0.0f;
    if (callbacks_.show_latency_monitor) {
        const auto right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const auto width = buttonWidth("Latency Monitor");
        if (right - ImGui::GetItemRectMax().x >= width + ImGui::GetStyle().ItemSpacing.x)
            ImGui::SameLine();
        if (ImGui::Button("Latency Monitor", ImVec2(width, 0.0f)))
            callbacks_.show_latency_monitor();
    }

    const float available = ImGui::GetContentRegionAvail().x;
    const auto& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float stripPadding = 9.0f * uiScale;
    const float minStripWidth = 2.0f * std::max(buttonWidth("M"), buttonWidth("S")) + spacing + 2.0f * stripPadding;
    const float preferredStripWidth = std::max(kStripWidth * uiScale, minStripWidth);
    const float stripWidth = std::min(preferredStripWidth, std::max(minStripWidth, (available - spacing) * 0.5f));
    const float minimumDetailsHeight = 120.0f * uiScale;
    const float stripControlsHeight = 5.0f * ImGui::GetFrameHeightWithSpacing() +
        3.0f * ImGui::GetTextLineHeightWithSpacing() + 2.0f * stripPadding;
    const float meterHeight = std::clamp(ImGui::GetContentRegionAvail().y - stripControlsHeight
        - style.ScrollbarSize - style.ItemSpacing.y - minimumDetailsHeight, 60.0f * uiScale, 140.0f * uiScale);
    const float stripHeight = meterHeight + stripControlsHeight;
    const float tracksWidth = hasMaster ? std::max(1.0f, available - stripWidth - spacing) : available;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(stripPadding, stripPadding));
    if (ImGui::BeginChild("Tracks", ImVec2(tracksWidth, stripHeight + ImGui::GetStyle().ScrollbarSize),
        ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (regularCount == 0)
            ImGui::TextDisabled("No tracks");
        for (size_t index = 0; index < regularCount; ++index) {
            auto& strip = strips[index];
            ImGui::PushID(strip.key.c_str());
            if (index > 0)
                ImGui::SameLine(0.0f, 0.0f);
            if (ImGui::BeginChild("Strip", ImVec2(stripWidth, stripHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                renderStrip(*engine, strip, meterHeight, uiScale);
            ImGui::EndChild();
            if (index + 1 < regularCount) {
                const auto top = ImGui::GetItemRectMin();
                const auto end = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(end.x - 1.0f, top.y), ImVec2(end.x - 1.0f, end.y),
                    ImGui::GetColorU32(ImGuiCol_Border));
            }
            if (focus_track_ == strip.index)
                ImGui::SetScrollHereX(0.5f);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (hasMaster) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(darkMixer() ? 0x262d36 : 0xeff3f7));
        if (ImGui::BeginChild("Master", ImVec2(stripWidth, stripHeight), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            renderStrip(*engine, strips.back(), meterHeight, uiScale);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar();
    focus_track_.reset();

    const auto detail = std::ranges::find(strips, output_track_, &Strip::key);
    if (detail != strips.end()) {
        ImGui::PushID(detail->key.c_str());
        const auto detailHeight = std::max(minimumDetailsHeight, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginChild("Track outputs", ImVec2(0.0f, detailHeight), ImGuiChildFlags_Borders))
            renderOutputs(*engine, *detail, uiScale);
        ImGui::EndChild();
        ImGui::PopID();
    }
    reveal_output_.reset();
    if (!gesture_active_)
        finishGesture(&commands.history());
    ImGui::PopStyleVar(2);
    ImGui::End();
}

}
