#pragma once

#include <vector>
#include <string>
#include <functional>
#include <imgui.h>
#include <uapmd/uapmd.hpp>

namespace uapmd::gui {

class ParameterList {
public:
    using ParameterChangeCallback = std::function<void(uint32_t parameterIndex, float value)>;
    using GetParameterValueStringCallback = std::function<std::string(uint32_t parameterIndex, float value)>;

private:
    std::vector<ParameterMetadata> parameters_;
    std::vector<float> parameterValues_;
    std::vector<std::string> parameterValueStrings_;
    char parameterFilter_[256] = "";
    bool reflectEventOut_ = true;

    ParameterChangeCallback onParameterChanged_;
    GetParameterValueStringCallback onGetParameterValueString_;

public:
    ParameterList();

    void setParameters(const std::vector<ParameterMetadata>& parameters);
    void setParameterValue(size_t index, float value);
    void setParameterValueString(size_t index, const std::string& valueString);

    void render();

    void setReflectEventOut(bool reflect);
    bool getReflectEventOut() const;

    void setOnParameterChanged(ParameterChangeCallback callback);
    void setOnGetParameterValueString(GetParameterValueStringCallback callback);

    const std::vector<ParameterMetadata>& getParameters() const;
    const std::vector<float>& getParameterValues() const;
    float getParameterValue(size_t index) const;

    void clearFilter();
    const char* getFilter() const;

private:
    std::vector<size_t> filterAndBuildIndices();
    void sortIndices(std::vector<size_t>& indices);
};

}
