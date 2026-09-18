// A font and bitmap implementation for LICE that needs nothing from the platform.
//
// LICE draws everything into memory except two things: LICE_SysBitmap, which is backed by
// a device context, and LICE_CachedFont, which rasterises glyphs through one. Those two
// are the only reason the JSFX graphics stack pulls in SWELL, and SWELL does not reach
// Android, iOS or the web. Supplying both here is what lets the editor build there.
//
// Everything else in LICE -- every primitive, blit, warp and effect -- is already portable
// and is used unchanged.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "WDL/lice/lice.h"
#include "WDL/lice/lice_text.h"

// swell-types.h, which LICE pulls in for the Win32 types, defines min and max as macros.
// They break std::min and std::max, so they go once the LICE headers are in.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "JsfxFontBackend.hpp"

namespace {

    // The font the host gave us, if any. JSFX asks for fonts by family name, but there is
    // no font enumeration on the platforms this file exists for, so one face serves for
    // all of them -- which is what REAPER effectively gets too, since almost every script
    // asks for whatever the system calls Arial.
    std::mutex g_font_mutex{};
    std::vector<uint8_t> g_font_data{};

    // What ysfx's CreateFont() asked for. It is filled in, selected into a (null) device
    // context, measured, and then handed to SetFromHFont, all on one thread inside
    // gfx_setfont, so one per thread is enough and no locking is needed.
    struct FontRequest {
        int height{12};
        int weight{400};
        bool italic{false};
        bool underline{false};
        std::string face{};
    };

    thread_local FontRequest* t_selected_font = nullptr;

    std::vector<FontRequest*>& ownedRequests() {
        static std::vector<FontRequest*> instance{};
        return instance;
    }
    std::mutex g_requests_mutex{};
}

namespace uapmd_jsfx {

    void setJsfxFontData(const void* data, size_t size) {
        std::lock_guard lock{g_font_mutex};
        g_font_data.clear();
        if (data && size > 0) {
            auto* bytes = static_cast<const uint8_t*>(data);
            g_font_data.assign(bytes, bytes + size);
        }
    }

    bool hasJsfxFontData() {
        std::lock_guard lock{g_font_mutex};
        return !g_font_data.empty();
    }

}

// ---------------------------------------------------------------------------------------
// LICE_SysBitmap without a device context
//
// The class is declared in lice.h with its members; only these three are out of line, so
// defining them here replaces the version that _LICE_NO_SYSBITMAPS_ compiles out. getDC()
// returns null, which is what LICE_IBitmap documents for a bitmap that is only memory.

LICE_SysBitmap::LICE_SysBitmap(int w, int h) {
    m_width = m_height = m_allocw = m_alloch = 0;
    m_bits = nullptr;
    m_dc = nullptr;
    m_draw_scaling = m_adv_scaling = 0;
    if (w > 0 && h > 0)
        __resize(w, h);
}

LICE_SysBitmap::~LICE_SysBitmap() {
    free(m_bits);
}

bool LICE_SysBitmap::__resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == m_width && h == m_height && m_bits)
        return false;
    if (w > m_allocw || h > m_alloch || !m_bits) {
        free(m_bits);
        m_allocw = w;
        m_alloch = h;
        m_bits = static_cast<LICE_pixel*>(calloc(static_cast<size_t>(w) * h, sizeof(LICE_pixel)));
    }
    m_width = w;
    m_height = h;
    return true;
}

// ---------------------------------------------------------------------------------------
// The handful of GDI entry points ysfx calls while setting up a font.
//
// These have C++ linkage because that is how swell.h declares them. They are not an
// emulation of GDI: they carry the font description from CreateFont() through to
// SetFromHFont(), and answer the one measurement ysfx asks for.

HFONT CreateFont(int height, int, int, int, int weight, char italic, char underline, char,
                 char, char, char, char, char, const char* face) {
    auto* request = new FontRequest{};
    // A negative height is GDI's way of asking for a character height rather than a cell
    // height. The difference does not survive a single font face, so it is ignored.
    request->height = height < 0 ? -height : height;
    if (request->height < 1)
        request->height = 12;
    request->weight = weight;
    request->italic = italic != 0;
    request->underline = underline != 0;
    request->face = face ? face : "";
    {
        std::lock_guard lock{g_requests_mutex};
        ownedRequests().emplace_back(request);
    }
    return reinterpret_cast<HFONT>(request);
}

HGDIOBJ SelectObject(HDC, HGDIOBJ obj) {
    auto* previous = reinterpret_cast<HGDIOBJ>(t_selected_font);
    t_selected_font = reinterpret_cast<FontRequest*>(obj);
    return previous;
}

BOOL GetTextMetrics(HDC, TEXTMETRIC* tm) {
    if (!tm)
        return 0;
    memset(tm, 0, sizeof(*tm));
    // ysfx reads tmHeight and uses it as the line height for the whole script, so it has
    // to be the height the glyphs are actually drawn at.
    tm->tmHeight = t_selected_font ? t_selected_font->height : 12;
    return 1;
}

int GetTextFace(HDC, int capacity, char* buffer) {
    if (!buffer || capacity < 1)
        return 0;
    // The name the script asked for is reported back rather than the name of the face
    // actually used: a script that compares them is checking whether its request was
    // honoured, and here it always is, by the only face there is.
    const std::string& face = t_selected_font ? t_selected_font->face : std::string{};
    const int n = std::min(static_cast<int>(face.size()), capacity - 1);
    memcpy(buffer, face.c_str(), static_cast<size_t>(n));
    buffer[n] = 0;
    return n;
}

int GetSysColor(int index) {
    // JSFX reads these through gfx_getsyscol() to match the host's look. There is no
    // system palette to consult here, so a neutral dark scheme is reported.
    switch (index) {
        case 5:  return 0x1E1E1E;   // COLOR_WINDOW
        case 8:  return 0xDCDCDC;   // COLOR_WINDOWTEXT
        case 13: return 0x4A3A6A;   // COLOR_HIGHLIGHT
        case 14: return 0xFFFFFF;   // COLOR_HIGHLIGHTTEXT
        case 15: return 0x2A2A2A;   // COLOR_3DFACE
        case 18: return 0xDCDCDC;   // COLOR_BTNTEXT
        default: return 0x808080;
    }
}

// ---------------------------------------------------------------------------------------
// LICE_IFont over stb_truetype

namespace {

    struct Glyph {
        std::vector<uint8_t> coverage{};   // width*height, 0..255
        int width{0};
        int height{0};
        int offsetX{0};
        int offsetY{0};    // from the baseline, positive downwards
        int advance{0};
    };

    class StbFont final : public LICE_IFont {
        stbtt_fontinfo info_{};
        bool ready_{false};
        float scale_{1.0f};
        int ascent_{0};
        int descent_{0};
        int line_gap_{0};
        int pixel_height_{12};
        int flags_{0};
        bool bold_{false};

        LICE_pixel text_color_{static_cast<LICE_pixel>(LICE_RGBA(255, 255, 255, 255))};
        LICE_pixel bk_color_{0};
        LICE_pixel effect_color_{0};
        int bk_mode_{TRANSPARENT};
        int combine_mode_{LICE_BLIT_MODE_COPY};
        float alpha_{1.0f};
        int line_spacing_adjust_{0};
        HFONT hfont_{nullptr};

        std::map<int, Glyph> glyphs_{};
        std::vector<uint8_t> font_bytes_{};

        const Glyph* glyph(int codepoint) {
            auto it = glyphs_.find(codepoint);
            if (it != glyphs_.end())
                return &it->second;
            if (!ready_)
                return nullptr;

            Glyph g{};
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&info_, codepoint, &advance, &bearing);
            g.advance = static_cast<int>(std::lround(advance * scale_));

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(&info_, codepoint, scale_, scale_, &x0, &y0, &x1, &y1);
            g.width = x1 - x0;
            g.height = y1 - y0;
            g.offsetX = x0;
            g.offsetY = y0;
            if (g.width > 0 && g.height > 0) {
                g.coverage.assign(static_cast<size_t>(g.width) * g.height, 0);
                stbtt_MakeCodepointBitmap(&info_, g.coverage.data(), g.width, g.height,
                                          g.width, scale_, scale_, codepoint);
                if (bold_)
                    emboldenInPlace(g);
            }
            auto inserted = glyphs_.emplace(codepoint, std::move(g));
            return &inserted.first->second;
        }

        // There is one weight available, so a bold request is met by smearing the coverage
        // one pixel sideways. It is not a real bold, but it reads as heavier, which is what
        // a script asking for bold wants.
        static void emboldenInPlace(Glyph& g) {
            std::vector<uint8_t> widened(static_cast<size_t>(g.width + 1) * g.height, 0);
            for (int y = 0; y < g.height; y++) {
                for (int x = 0; x < g.width; x++) {
                    const uint8_t v = g.coverage[static_cast<size_t>(y) * g.width + x];
                    auto& a = widened[static_cast<size_t>(y) * (g.width + 1) + x];
                    auto& b = widened[static_cast<size_t>(y) * (g.width + 1) + x + 1];
                    a = std::max<uint8_t>(a, v);
                    b = std::max<uint8_t>(b, v);
                }
            }
            g.coverage.swap(widened);
            g.width += 1;
            g.advance += 1;
        }

        // Decodes one UTF-8 sequence, falling back to treating the byte as Latin-1 when the
        // sequence is malformed. JSFX in the wild is not reliably UTF-8.
        static int nextCodepoint(const char* str, int len, int& index) {
            const auto b0 = static_cast<uint8_t>(str[index]);
            auto continuation = [&](int offset) {
                return index + offset < len &&
                       (static_cast<uint8_t>(str[index + offset]) & 0xC0) == 0x80;
            };
            if (b0 < 0x80) {
                index += 1;
                return b0;
            }
            if ((b0 & 0xE0) == 0xC0 && continuation(1)) {
                const int cp = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(str[index + 1]) & 0x3F);
                index += 2;
                return cp;
            }
            if ((b0 & 0xF0) == 0xE0 && continuation(1) && continuation(2)) {
                const int cp = ((b0 & 0x0F) << 12) |
                               ((static_cast<uint8_t>(str[index + 1]) & 0x3F) << 6) |
                               (static_cast<uint8_t>(str[index + 2]) & 0x3F);
                index += 3;
                return cp;
            }
            index += 1;
            return b0;
        }

        int textWidth(const char* str, int len) {
            int width = 0;
            for (int i = 0; i < len;) {
                const int cp = nextCodepoint(str, len, i);
                if (const auto* g = glyph(cp))
                    width += g->advance;
            }
            return width;
        }

        void blendGlyph(LICE_IBitmap* bm, const Glyph& g, int penX, int baselineY,
                        const RECT* clip) {
            if (g.coverage.empty())
                return;
            LICE_pixel* bits = bm->getBits();
            if (!bits)
                return;
            const int bmw = bm->getWidth();
            const int bmh = bm->getHeight();
            int span = bm->getRowSpan();
            int flip = 0;
            if (bm->isFlipped())
                flip = 1;

            const int red = LICE_GETR(text_color_);
            const int green = LICE_GETG(text_color_);
            const int blue = LICE_GETB(text_color_);

            for (int gy = 0; gy < g.height; gy++) {
                const int y = baselineY + g.offsetY + gy;
                if (y < 0 || y >= bmh)
                    continue;
                if (clip && (y < clip->top || y >= clip->bottom))
                    continue;
                LICE_pixel* row = bits + (flip ? (bmh - 1 - y) : y) * span;
                for (int gx = 0; gx < g.width; gx++) {
                    const int x = penX + g.offsetX + gx;
                    if (x < 0 || x >= bmw)
                        continue;
                    if (clip && (x < clip->left || x >= clip->right))
                        continue;
                    const int coverage = g.coverage[static_cast<size_t>(gy) * g.width + gx];
                    if (coverage == 0)
                        continue;
                    const int a = static_cast<int>(coverage * alpha_);
                    if (a <= 0)
                        continue;
                    LICE_pixel* dst = row + x;
                    auto* chan = reinterpret_cast<LICE_pixel_chan*>(dst);
                    chan[LICE_PIXEL_R] = static_cast<LICE_pixel_chan>(
                            (chan[LICE_PIXEL_R] * (255 - a) + red * a) / 255);
                    chan[LICE_PIXEL_G] = static_cast<LICE_pixel_chan>(
                            (chan[LICE_PIXEL_G] * (255 - a) + green * a) / 255);
                    chan[LICE_PIXEL_B] = static_cast<LICE_pixel_chan>(
                            (chan[LICE_PIXEL_B] * (255 - a) + blue * a) / 255);
                    chan[LICE_PIXEL_A] = static_cast<LICE_pixel_chan>(
                            std::max<int>(chan[LICE_PIXEL_A], a));
                }
            }
        }

    public:
        StbFont() {
            std::lock_guard lock{g_font_mutex};
            font_bytes_ = g_font_data;
        }

        void SetFromHFont(HFONT font, int flags) override {
            hfont_ = font;
            flags_ = flags;
            glyphs_.clear();
            ready_ = false;

            auto* request = reinterpret_cast<FontRequest*>(font);
            pixel_height_ = request && request->height > 0 ? request->height : 12;
            bold_ = request && request->weight >= 600;

            if (font_bytes_.empty())
                return;
            const int offset = stbtt_GetFontOffsetForIndex(font_bytes_.data(), 0);
            if (offset < 0 || !stbtt_InitFont(&info_, font_bytes_.data(), offset))
                return;

            // JSFX asks for a cell height, which is the whole line, so the glyphs are
            // scaled to fit ascent-to-descent rather than to that height directly.
            scale_ = stbtt_ScaleForPixelHeight(&info_, static_cast<float>(pixel_height_));
            stbtt_GetFontVMetrics(&info_, &ascent_, &descent_, &line_gap_);
            ready_ = true;
        }

        LICE_pixel SetTextColor(LICE_pixel color) override {
            auto previous = text_color_;
            text_color_ = color;
            return previous;
        }
        LICE_pixel SetBkColor(LICE_pixel color) override {
            auto previous = bk_color_;
            bk_color_ = color;
            return previous;
        }
        LICE_pixel SetEffectColor(LICE_pixel color) override {
            auto previous = effect_color_;
            effect_color_ = color;
            return previous;
        }
        int SetBkMode(int mode) override {
            auto previous = bk_mode_;
            bk_mode_ = mode;
            return previous;
        }
        void SetCombineMode(int combine, float alpha) override {
            combine_mode_ = combine;
            alpha_ = alpha;
        }
        LICE_pixel GetTextColor() override { return text_color_; }
        HFONT GetHFont() override { return hfont_; }
        int GetLineHeight() override { return pixel_height_ + line_spacing_adjust_; }
        void SetLineSpacingAdjust(int amt) override { line_spacing_adjust_ = amt; }

        int DrawText(LICE_IBitmap* bm, const char* str, int strcnt, RECT* rect,
                     UINT dtFlags) override {
            if (!str || !rect)
                return 0;
            int len = strcnt;
            if (len < 0)
                len = static_cast<int>(strlen(str));

            const int lineHeight = GetLineHeight();
            if (!ready_) {
                // With no font loaded there is nothing to measure or draw. Reporting the
                // line height still lets a caller lay out, and ysfx falls back to LICE's
                // built-in font when LICE_CreateFont() returns null in the first place.
                if (dtFlags & DT_CALCRECT) {
                    rect->right = rect->left;
                    rect->bottom = rect->top + lineHeight;
                }
                return lineHeight;
            }

            const int width = textWidth(str, len);
            if (dtFlags & DT_CALCRECT) {
                rect->right = rect->left + width;
                rect->bottom = rect->top + lineHeight;
                return lineHeight;
            }
            if (!bm)
                return lineHeight;

            int x = rect->left;
            int y = rect->top;
            if (dtFlags & DT_CENTER)
                x += ((rect->right - rect->left) - width) / 2;
            else if (dtFlags & DT_RIGHT)
                x += (rect->right - rect->left) - width;
            if (dtFlags & DT_VCENTER)
                y += ((rect->bottom - rect->top) - lineHeight) / 2;
            else if (dtFlags & DT_BOTTOM)
                y += (rect->bottom - rect->top) - lineHeight;

            const bool clipped = (dtFlags & DT_NOCLIP) == 0;
            const int baseline = y + static_cast<int>(std::lround(ascent_ * scale_));

            if (bk_mode_ == OPAQUE) {
                RECT fill{x, y, x + width, y + lineHeight};
                LICE_FillRect(bm, fill.left, fill.top, fill.right - fill.left,
                              fill.bottom - fill.top, bk_color_, alpha_, LICE_BLIT_MODE_COPY);
            }

            int pen = x;
            for (int i = 0; i < len;) {
                const int cp = nextCodepoint(str, len, i);
                const auto* g = glyph(cp);
                if (!g)
                    continue;
                blendGlyph(bm, *g, pen, baseline, clipped ? rect : nullptr);
                pen += g->advance;
            }

            if (underlineRequested()) {
                const int uy = baseline + 1;
                LICE_FillRect(bm, x, uy, width, 1, text_color_, alpha_, LICE_BLIT_MODE_COPY);
            }

            rect->right = pen;
            return lineHeight;
        }

    private:
        bool underlineRequested() const {
            auto* request = reinterpret_cast<FontRequest*>(hfont_);
            return request && request->underline;
        }
    };

}

// The hook ysfx calls, patched in through YSFX_EXTERNAL_FONT. Returning null is a
// supported answer: ysfx then draws text with LICE's built-in bitmap font, which needs
// nothing at all. That is what happens when the host has not supplied any font data.
LICE_IFont* ysfx_create_external_lice_font() {
    if (!uapmd_jsfx::hasJsfxFontData())
        return nullptr;
    return new StbFont();
}
