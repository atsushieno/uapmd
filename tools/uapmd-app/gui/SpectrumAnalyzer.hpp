#pragma once

#include <imgui.h>
#include <vector>
#include <functional>

namespace uapmd::gui {
    class SpectrumAnalyzer {
    public:
        static constexpr int kDefaultBars = 32;
        using DataProvider = std::function<void(float* data, int dataSize)>;

    private:
        int numBars_;
        std::vector<float> spectrum_;
        ImVec2 size_;
        DataProvider dataProvider_;

    public:
        explicit SpectrumAnalyzer(int numBars = kDefaultBars);

        void setSize(ImVec2 size);
        void setNumBars(int numBars);
        void setDataProvider(DataProvider provider);
        void render(const char* label = nullptr);

    private:
        void ensureSpectrumSize();
        void updateSpectrum();
    };
}
