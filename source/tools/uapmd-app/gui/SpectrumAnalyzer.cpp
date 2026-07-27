#include "SpectrumAnalyzer.hpp"
#include <algorithm>
#include <cmath>

namespace uapmd::gui {

SpectrumAnalyzer::SpectrumAnalyzer(int numBars)
    : numBars_(numBars), size_(ImVec2(200.0f, 64.0f)) {
    ensureSpectrumSize();
}

void SpectrumAnalyzer::setSize(ImVec2 size) {
    size_ = size;
}

void SpectrumAnalyzer::setNumBars(int numBars) {
    if (numBars > 0 && numBars != numBars_) {
        numBars_ = numBars;
        ensureSpectrumSize();
    }
}

void SpectrumAnalyzer::setDataProvider(DataProvider provider) {
    dataProvider_ = provider;
}

void SpectrumAnalyzer::render(const char* label) {
    ensureSpectrumSize();
    updateSpectrum();
    const char* displayLabel = label ? label : "##Spectrum";
    ImGui::PlotHistogram(displayLabel, spectrum_.data(), numBars_, 0, nullptr, 0.0f, 1.0f, size_);
}

void SpectrumAnalyzer::ensureSpectrumSize() {
    if (static_cast<int>(spectrum_.size()) != numBars_) {
        spectrum_.resize(numBars_, 0.0f);
    }
    if (static_cast<int>(time_domain_.size()) != kTimeDomainSamples)
        time_domain_.resize(kTimeDomainSamples, 0.0f);
}

void SpectrumAnalyzer::updateSpectrum() {
    if (!dataProvider_)
        return;
    dataProvider_(time_domain_.data(), kTimeDomainSamples);
    for (int bar = 0; bar < numBars_; ++bar) {
        const auto firstSample = bar * kTimeDomainSamples / numBars_;
        const auto endSample = (bar + 1) * kTimeDomainSamples / numBars_;
        float sum = 0.0f;
        for (int sample = firstSample; sample < endSample; ++sample)
            sum += std::abs(time_domain_[sample]);
        spectrum_[bar] = sum / static_cast<float>(std::max(endSample - firstSample, 1));
    }
}

}
