#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <uapmd/uapmd.hpp>

#include "../../remidy-imgui-shared/MidiKeyboard.hpp"

namespace uapmd::gui {

class ParameterList {
public:
    enum class ParameterContext {
        Global = 0,
        Group,
        Channel,
        Key,
    };

    using ParameterChangeCallback = std::function<void(uint32_t parameterIndex, float value)>;
    using GetParameterValueStringCallback = std::function<std::string(uint32_t parameterIndex, float value)>;
    using ContextChangeCallback = std::function<void(ParameterContext context, uint8_t value)>;

private:
    std::vector<ParameterMetadata> parameters_;
    std::vector<float> parameterValues_;
    std::vector<std::string> parameterValueStrings_;
    char parameterFilter_[256] = "";

    ParameterChangeCallback onParameterChanged_;
    GetParameterValueStringCallback onGetParameterValueString_;
    ContextChangeCallback onContextChanged_;

    ParameterContext context_ = ParameterContext::Global;
    int contextValue_ = 64;
    MidiKeyboard perNoteKeyboard_;
    std::array<std::string, 128> contextValueLabels_{};

public:
    ParameterList();
    ParameterList(const ParameterList& other);
    ParameterList& operator=(const ParameterList& other);
    ParameterList(ParameterList&& other) noexcept;
    ParameterList& operator=(ParameterList&& other) noexcept;

    void setParameters(const std::vector<ParameterMetadata>& parameters);
    void setParameterValue(size_t index, float value);
    void setParameterValueString(size_t index, const std::string& valueString);

    void render();

    void setOnParameterChanged(ParameterChangeCallback callback);
    void setOnGetParameterValueString(GetParameterValueStringCallback callback);
    void setOnContextChanged(ContextChangeCallback callback);

    const std::vector<ParameterMetadata>& getParameters() const;
    const std::vector<float>& getParameterValues() const;
    float getParameterValue(size_t index) const;
    ParameterContext context() const;
    uint8_t contextValue() const;
    void setContext(ParameterContext context);
    void setContextValue(uint8_t value);

    void clearFilter();
    const char* getFilter() const;

private:
    std::vector<size_t> filterAndBuildIndices();
    void sortIndices(std::vector<size_t>& indices);
    void notifyContextChanged();
    void bindKeyboardCallback();
};

}
