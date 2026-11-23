#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace remidy_imgui {

enum class ThemeMode {
	Dark,
	Light
};

inline void SetupImGuiStyle(ThemeMode mode = ThemeMode::Dark)
{
	ImGuiStyle& style = ImGui::GetStyle();

	style.Alpha = 1.0f;
	style.DisabledAlpha = 0.5f;
	style.WindowPadding = ImVec2(10.0f, 8.0f);
	style.WindowRounding = 7.0f;
	style.WindowBorderSize = 0.0f;
	style.WindowMinSize = ImVec2(20.0f, 20.0f);
	style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	style.WindowMenuButtonPosition = ImGuiDir_Left;
	style.ChildRounding = 6.0f;
	style.ChildBorderSize = 0.0f;
	style.PopupRounding = 6.0f;
	style.PopupBorderSize = 1.0f;
	style.FramePadding = ImVec2(10.0f, 7.0f);
	style.FrameRounding = 5.0f;
	style.FrameBorderSize = 0.0f;
	style.ItemSpacing = ImVec2(8.0f, 6.0f);
	style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
	style.CellPadding = ImVec2(4.0f, 2.0f);
	style.IndentSpacing = 21.0f;
	style.ColumnsMinSpacing = 6.0f;
#if defined(__ANDROID__)
	style.ScrollbarSize = 20.0f; // touch-friendly without consuming the editor
#else
	style.ScrollbarSize = 14.0f;
#endif
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
	style.TouchExtraPadding = ImVec2(8.0f, 8.0f); // makes small widgets such as titlebar close buttons touchable
#endif
	style.ScrollbarRounding = 9.0f;
	style.GrabMinSize = 10.0f;
	style.GrabRounding = 0.0f;
	style.TabRounding = 6.0f;
	style.TabBorderSize = 0.0f;
	style.TabCloseButtonMinWidthSelected = 0.0f;
	style.TabCloseButtonMinWidthUnselected = 0.0f;
	style.ColorButtonPosition = ImGuiDir_Right;
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

	// Color scheme based on theme mode
	if (mode == ThemeMode::Dark) {
		style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.58431375f, 0.59607846f, 0.6156863f, 1.0f);
		style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0627451f, 0.06666667f, 0.08627451f, 1.0f);
		style.Colors[ImGuiCol_ChildBg] = ImVec4(0.043137256f, 0.047058824f, 0.05882353f, 1.0f);
		style.Colors[ImGuiCol_PopupBg] = ImVec4(0.043137256f, 0.047058824f, 0.05882353f, 1.0f);
		style.Colors[ImGuiCol_Border] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
		style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.16f, 0.16f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.2f, 0.3f, 1.0f);
	} else {
		// Light theme colors
		style.Colors[ImGuiCol_Text] = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
		style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
		style.Colors[ImGuiCol_WindowBg] = ImVec4(0.95f, 0.95f, 0.96f, 1.0f);
		style.Colors[ImGuiCol_ChildBg] = ImVec4(0.98f, 0.98f, 0.99f, 1.0f);
		style.Colors[ImGuiCol_PopupBg] = ImVec4(0.98f, 0.98f, 0.99f, 1.0f);
		style.Colors[ImGuiCol_Border] = ImVec4(0.7f, 0.7f, 0.75f, 1.0f);
		style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.85f, 0.85f, 0.88f, 1.0f);
		style.Colors[ImGuiCol_FrameBg] = ImVec4(0.88f, 0.88f, 0.90f, 1.0f);
	}
	if (mode == ThemeMode::Dark) {
		style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.96f, 0.43f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.96f, 0.43f, 0.16f, 0.0f);
		style.Colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
		style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.82f, 0.31f, 0.10f, 1.0f);
		style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
		style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.043137256f, 0.047058824f, 0.05882353f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.043137256f, 0.047058824f, 0.05882353f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.10980392f, 0.11372549f, 0.13333334f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.14509805f, 0.14901961f, 0.18431373f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.4862745f, 0.4862745f, 0.4862745f, 1.0f);
		style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.22746783f);
		style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.66f, 0.38f, 0.3304721f);
	} else {
		style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.82f, 0.82f, 0.85f, 1.0f);
		style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.88f, 0.88f, 0.90f, 1.0f);
		style.Colors[ImGuiCol_TitleBg] = ImVec4(0.88f, 0.88f, 0.90f, 1.0f);
		style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.96f, 0.43f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.88f, 0.88f, 0.90f, 1.0f);
		style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.93f, 0.93f, 0.95f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.93f, 0.93f, 0.95f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.75f, 0.75f, 0.78f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.65f, 0.65f, 0.70f, 1.0f);
		style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
		style.Colors[ImGuiCol_CheckMark] = ImVec4(0.82f, 0.31f, 0.10f, 1.0f);
		style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.82f, 0.31f, 0.10f, 0.7f);
		style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.96f, 0.43f, 0.16f, 0.9f);
	}
	if (mode == ThemeMode::Dark) {
		style.Colors[ImGuiCol_Button] = ImVec4(0.82f, 0.31f, 0.10f, 1.0f);
		style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 0.52f, 0.24f, 1.0f);
		style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.96f, 0.43f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_Header] = ImVec4(0.96f, 0.43f, 0.16f, 0.31f);
		style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.96f, 0.43f, 0.16f, 0.8f);
		style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.96f, 0.43f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_Separator] = ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 0.62f);
		style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.82f, 0.31f, 0.10f, 0.78f);
		style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.82f, 0.31f, 0.10f, 1.0f);
		style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 0.17f);
		style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.96f, 0.43f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.96f, 0.43f, 0.16f, 0.95f);
	} else {
		style.Colors[ImGuiCol_Button] = ImVec4(0.96f, 0.43f, 0.16f, 1.0f);
		style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 0.58f, 0.30f, 1.0f);
		style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.82f, 0.31f, 0.10f, 1.0f);
		style.Colors[ImGuiCol_Header] = ImVec4(0.96f, 0.43f, 0.16f, 0.25f);
		style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.96f, 0.43f, 0.16f, 0.6f);
		style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.96f, 0.43f, 0.16f, 0.8f);
		style.Colors[ImGuiCol_Separator] = ImVec4(0.65f, 0.65f, 0.68f, 0.62f);
		style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.82f, 0.31f, 0.10f, 0.78f);
		style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.82f, 0.31f, 0.10f, 1.0f);
		style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.7f, 0.7f, 0.73f, 0.4f);
		style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.96f, 0.43f, 0.16f, 0.8f);
		style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.96f, 0.43f, 0.16f, 0.95f);
	}
	if (mode == ThemeMode::Dark) {
		style.Colors[ImGuiCol_Tab] = ImVec4(0.82f, 0.31f, 0.10f, 0.931f);
		style.Colors[ImGuiCol_TabHovered] = ImVec4(0.96f, 0.43f, 0.16f, 0.8f);
		style.Colors[ImGuiCol_TabActive] = ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
		style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.91764706f, 0.9254902f, 0.93333334f, 0.9862f);
		style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.780f, 0.702f, 0.878f, 1.0f);
		style.Colors[ImGuiCol_PlotLines] = ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
		style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
		style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
		style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.44705883f, 0.0f, 1.0f);
		style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.851f, 0.749f, 0.949f, 0.5f);
		style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.5686275f, 0.5686275f, 0.6392157f, 1.0f);
		style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.6784314f, 0.6784314f, 0.7372549f, 1.0f);
		style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 0.09f);
		style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.96f, 0.43f, 0.16f, 0.35f);
		style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.96f, 0.43f, 0.16f, 0.95f);
		style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.96f, 0.43f, 0.16f, 0.8f);
		style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.7f);
		style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.2f);
		style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
	} else {
		style.Colors[ImGuiCol_Tab] = ImVec4(0.96f, 0.43f, 0.16f, 0.7f);
		style.Colors[ImGuiCol_TabHovered] = ImVec4(1.00f, 0.58f, 0.30f, 0.8f);
		style.Colors[ImGuiCol_TabActive] = ImVec4(0.95f, 0.95f, 0.96f, 1.0f);
		style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.85f, 0.85f, 0.87f, 0.9f);
		style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.88f, 0.85f, 0.92f, 1.0f);
		style.Colors[ImGuiCol_PlotLines] = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
		style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
		style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
		style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.44705883f, 0.0f, 1.0f);
		style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.780f, 0.702f, 0.878f, 0.8f);
		style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.6f, 0.6f, 0.65f, 1.0f);
		style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.75f, 0.75f, 0.78f, 1.0f);
		style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.3f, 0.3f, 0.35f, 0.06f);
		style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.96f, 0.43f, 0.16f, 0.35f);
		style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.96f, 0.43f, 0.16f, 0.95f);
		style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.96f, 0.43f, 0.16f, 0.8f);
		style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.4f, 0.4f, 0.4f, 0.7f);
		style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
		style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
	}
}

}