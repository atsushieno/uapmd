#include "FontLoader.hpp"
#include "FontIcons.hpp"

#include <AppFonts.h>
#include <ResEmbed/ResEmbed.h>
#include <IconsFontAwesome7.h>
#include <imgui.h>
#include <cstdio>

namespace uapmd::gui {

namespace {

constexpr float kBaseFontSize = 16.0f;

void mergeFontaudioIconFont(ImGuiIO& io, float baseFontSize) {
    static_assert(ICON_MIN_FAD <= ICON_MAX_FAD, "Invalid Fontaudio icon range");
    auto fontData = ResEmbed::get("fontaudio.ttf", "AppFonts");
    if (!fontData)
        return;

    ImFontConfig iconConfig{};
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.FontDataOwnedByAtlas = false;
    iconConfig.OversampleH = 1;
    iconConfig.OversampleV = 1;
    iconConfig.GlyphMinAdvanceX = baseFontSize * 0.65f;
    iconConfig.GlyphOffset = ImVec2(0.0f, baseFontSize * 0.05f);

    static const ImWchar fontaudioRanges[] = { ICON_MIN_FAD, ICON_MAX_FAD, 0 };
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fontData.data()),
        static_cast<int>(fontData.size()),
        baseFontSize,
        &iconConfig,
        fontaudioRanges
    );
}

void mergeFontAwesomeIconFont(ImGuiIO& io, float baseFontSize) {
    static_assert(ICON_MIN_FA <= ICON_MAX_FA, "Invalid Font Awesome icon range");
    auto fontData = ResEmbed::get("Font Awesome 7 Free-Solid-900.otf", "AppFonts");
    if (!fontData)
        return;

    ImFontConfig iconConfig{};
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.FontDataOwnedByAtlas = false;
    iconConfig.OversampleH = 1;
    iconConfig.OversampleV = 1;
    iconConfig.GlyphMinAdvanceX = baseFontSize * 0.65f;
    iconConfig.GlyphOffset = ImVec2(0.0f, baseFontSize * 0.05f);

    static const ImWchar fontAwesomeRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fontData.data()),
        static_cast<int>(fontData.size()),
        baseFontSize,
        &iconConfig,
        fontAwesomeRanges
    );
}

} // namespace

void ensureApplicationFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontDefault = nullptr;

    auto fontData = ResEmbed::get("Roboto-SemiBold.ttf", "AppFonts");
    if (fontData) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        config.FontDataOwnedByAtlas = false;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(fontData.data()),
            static_cast<int>(fontData.size()),
            kBaseFontSize,
            &config
        );
        if (font != nullptr) {
            io.FontDefault = font;
            mergeFontAwesomeIconFont(io, kBaseFontSize);
            mergeFontaudioIconFont(io, kBaseFontSize);
            return;
        }
    }

    std::fprintf(stderr, "uapmd-app: failed to load embedded font\n");
    ImFont* fallback = io.Fonts->AddFontDefault();
    io.FontDefault = fallback;
    mergeFontAwesomeIconFont(io, kBaseFontSize);
    mergeFontaudioIconFont(io, kBaseFontSize);
}

} // namespace uapmd::gui
