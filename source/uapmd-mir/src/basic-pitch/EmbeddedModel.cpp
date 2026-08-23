#include "OnnxWeights.hpp"

#include <cstddef>

// Generated at configure time from the downloaded ONNX file. The model is
// carried as hex text split into chunks rather than as one byte array: MSVC
// caps a string literal at 65535 bytes, and a 230 KB model would blow past
// that as a single literal.
extern const char* const kBasicPitchModelHexChunks[];
extern const size_t kBasicPitchModelHexChunkCount;
extern const size_t kBasicPitchModelByteCount;

namespace uapmd_basic_pitch {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> decodeModel() {
    std::vector<uint8_t> bytes;
    bytes.reserve(kBasicPitchModelByteCount);
    int high = -1;
    for (size_t chunk = 0; chunk < kBasicPitchModelHexChunkCount; ++chunk) {
        for (const char* cursor = kBasicPitchModelHexChunks[chunk]; *cursor; ++cursor) {
            const auto digit = hexDigit(*cursor);
            if (digit < 0)
                continue;
            if (high < 0) {
                high = digit;
                continue;
            }
            bytes.push_back(static_cast<uint8_t>((high << 4) | digit));
            high = -1;
        }
    }
    return bytes;
}

} // namespace

std::span<const uint8_t> embeddedModel() {
    // Decoding 230 KB of hex costs microseconds, and only the first caller
    // pays it.
    static const std::vector<uint8_t> bytes = decodeModel();
    return bytes;
}

} // namespace uapmd_basic_pitch
