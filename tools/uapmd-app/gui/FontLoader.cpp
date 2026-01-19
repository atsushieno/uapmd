#include "FontLoader.hpp"
#include "EmbeddedFont.hpp"
#include <imgui.h>
#include <zlib.h>
#include <vector>
#include <cstdio>

namespace uapmd::gui {

namespace {

std::vector<uint8_t> extractTtfFromZip(const uint8_t* zip_data, size_t zip_size) {
    constexpr uint32_t kEndOfCentralDirSig = 0x06054b50;
    constexpr uint32_t kCentralDirSig = 0x02014b50;
    constexpr uint32_t kLocalFileHeaderSig = 0x04034b50;

    if (zip_size < 22)
        return {};

    size_t eocd_offset = zip_size - 22;
    while (eocd_offset > 0) {
        if (*reinterpret_cast<const uint32_t*>(zip_data + eocd_offset) == kEndOfCentralDirSig)
            break;
        eocd_offset--;
    }

    if (eocd_offset == 0)
        return {};

    uint32_t central_dir_offset = *reinterpret_cast<const uint32_t*>(zip_data + eocd_offset + 16);
    if (central_dir_offset + 46 > zip_size)
        return {};

    const uint8_t* central_header = zip_data + central_dir_offset;
    if (*reinterpret_cast<const uint32_t*>(central_header) != kCentralDirSig)
        return {};

    uint16_t compression = *reinterpret_cast<const uint16_t*>(central_header + 10);
    uint32_t compressed_size = *reinterpret_cast<const uint32_t*>(central_header + 20);
    uint32_t uncompressed_size = *reinterpret_cast<const uint32_t*>(central_header + 24);
    uint32_t local_header_offset = *reinterpret_cast<const uint32_t*>(central_header + 42);

    if (local_header_offset + 30 > zip_size)
        return {};

    const uint8_t* local_header = zip_data + local_header_offset;
    if (*reinterpret_cast<const uint32_t*>(local_header) != kLocalFileHeaderSig)
        return {};

    uint16_t local_name_len = *reinterpret_cast<const uint16_t*>(local_header + 26);
    uint16_t local_extra_len = *reinterpret_cast<const uint16_t*>(local_header + 28);

    size_t data_offset = local_header_offset + 30 + local_name_len + local_extra_len;
    if (data_offset + compressed_size > zip_size)
        return {};

    const uint8_t* compressed_data = zip_data + data_offset;

    if (compression == 0)
        return std::vector<uint8_t>(compressed_data, compressed_data + uncompressed_size);
    else if (compression == 8) {
        std::vector<uint8_t> result(uncompressed_size);
        z_stream stream{};
        stream.next_in = const_cast<uint8_t*>(compressed_data);
        stream.avail_in = compressed_size;
        stream.next_out = result.data();
        stream.avail_out = uncompressed_size;

        if (inflateInit2(&stream, -15) != Z_OK)
            return {};

        int ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);

        if (ret == Z_STREAM_END)
            return result;
    }

    return {};
}

} // namespace

void ensureApplicationFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontDefault = nullptr;

    static std::vector<uint8_t> ttf_data = extractTtfFromZip(
        uapmd::app::kEmbeddedFontData,
        uapmd::app::kEmbeddedFontSize
    );
    if (!ttf_data.empty()) {
        constexpr float kBaseFontSize = 16.0f;
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        config.FontDataOwnedByAtlas = false;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            ttf_data.data(),
            static_cast<int>(ttf_data.size()),
            kBaseFontSize,
            &config
        );
        if (font != nullptr) {
            io.FontDefault = font;
            return;
        }
    }

    std::fprintf(stderr, "uapmd-app: failed to load embedded font\n");
    ImFont* fallback = io.Fonts->AddFontDefault();
    io.FontDefault = fallback;
}

} // namespace uapmd::gui
