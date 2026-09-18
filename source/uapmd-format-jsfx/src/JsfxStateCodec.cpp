#include "JsfxStateCodec.hpp"

#include <cstring>

namespace uapmd_jsfx {

    namespace {
        constexpr uint8_t kMagic[4] = {'J', 'S', 'F', 'X'};
        // magic + version + slider count, then the sliders, then the data size.
        constexpr size_t kHeaderSize = 4 + 4 + 4;
        constexpr size_t kSliderSize = 4 + 8;
        constexpr size_t kDataSizeSize = 8;

        void putU32(std::vector<uint8_t>& out, uint32_t value) {
            out.emplace_back(static_cast<uint8_t>(value));
            out.emplace_back(static_cast<uint8_t>(value >> 8));
            out.emplace_back(static_cast<uint8_t>(value >> 16));
            out.emplace_back(static_cast<uint8_t>(value >> 24));
        }

        void putU64(std::vector<uint8_t>& out, uint64_t value) {
            for (int i = 0; i < 8; i++)
                out.emplace_back(static_cast<uint8_t>(value >> (i * 8)));
        }

        void putDouble(std::vector<uint8_t>& out, double value) {
            uint64_t bits;
            static_assert(sizeof(bits) == sizeof(value), "double is not 64 bits");
            std::memcpy(&bits, &value, sizeof(bits));
            putU64(out, bits);
        }

        uint32_t getU32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0])
                 | (static_cast<uint32_t>(p[1]) << 8)
                 | (static_cast<uint32_t>(p[2]) << 16)
                 | (static_cast<uint32_t>(p[3]) << 24);
        }

        uint64_t getU64(const uint8_t* p) {
            uint64_t value = 0;
            for (int i = 0; i < 8; i++)
                value |= static_cast<uint64_t>(p[i]) << (i * 8);
            return value;
        }

        double getDouble(const uint8_t* p) {
            const uint64_t bits = getU64(p);
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
    }

    std::vector<uint8_t> encodeState(const ysfx_state_t& state) {
        std::vector<uint8_t> out{};
        out.reserve(kHeaderSize + state.slider_count * kSliderSize + kDataSizeSize + state.data_size);

        out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
        putU32(out, kStateVersion);

        putU32(out, state.slider_count);
        for (uint32_t i = 0; i < state.slider_count; i++) {
            putU32(out, state.sliders[i].index);
            putDouble(out, static_cast<double>(state.sliders[i].value));
        }

        putU64(out, static_cast<uint64_t>(state.data_size));
        if (state.data_size > 0 && state.data)
            out.insert(out.end(), state.data, state.data + state.data_size);

        return out;
    }

    bool decodeState(const std::vector<uint8_t>& bytes, DecodedState& out) {
        out.sliders.clear();
        out.data.clear();

        // A plugin that was never edited has no state to restore, which is not an error.
        if (bytes.empty())
            return true;

        if (bytes.size() < kHeaderSize)
            return false;
        if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
            return false;
        if (getU32(bytes.data() + 4) != kStateVersion)
            return false;

        const uint32_t sliderCount = getU32(bytes.data() + 8);
        // Guard against a corrupt count before reserving anything for it.
        if (bytes.size() < kHeaderSize + static_cast<size_t>(sliderCount) * kSliderSize + kDataSizeSize)
            return false;

        size_t offset = kHeaderSize;
        out.sliders.reserve(sliderCount);
        for (uint32_t i = 0; i < sliderCount; i++) {
            ysfx_state_slider_t slider{};
            slider.index = getU32(bytes.data() + offset);
            slider.value = static_cast<ysfx_real>(getDouble(bytes.data() + offset + 4));
            out.sliders.emplace_back(slider);
            offset += kSliderSize;
        }

        const uint64_t dataSize = getU64(bytes.data() + offset);
        offset += kDataSizeSize;
        if (dataSize > bytes.size() - offset)
            return false;

        out.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + dataSize));
        return true;
    }

}
