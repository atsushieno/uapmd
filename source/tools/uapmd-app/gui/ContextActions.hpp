#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace uapmd::gui {

inline bool contextActionButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f), const char* tooltip = nullptr)
{
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::Button(label, size);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

inline bool contextActionArrowButton(const char* label, ImGuiDir dir)
{
    ImGui::SetNextItemAllowOverlap();
    return ImGui::ArrowButton(label, dir);
}

inline bool contextActionMenuItem(const char* label, bool selected = false)
{
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    const bool clicked = ImGui::Selectable(label, selected,
                                           ImGuiSelectableFlags_SelectOnRelease |
                                               ImGuiSelectableFlags_NoSetKeyOwner |
                                               ImGuiSelectableFlags_SetNavIdOnHover,
                                           ImVec2(0.0f, ImGui::GetFrameHeight()));
    ImGui::PopStyleVar();
    return clicked;
}

} // namespace uapmd::gui
