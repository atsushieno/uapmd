#include "ParameterList.hpp"
#include <algorithm>
#include <iostream>
#include <cmath>
#include <cfloat>

namespace uapmd::gui {

ParameterList::ParameterList() {
    std::fill(std::begin(parameterFilter_), std::end(parameterFilter_), '\0');
    for (size_t i = 0; i < contextValueLabels_.size(); ++i)
        contextValueLabels_[i] = std::to_string(i);

    perNoteKeyboard_.setOctaveRange(4, 2);
    perNoteKeyboard_.setKeySize(14.0f, 50.0f, 32.0f);
    bindKeyboardCallback();
    perNoteKeyboard_.setHighlightedKey(contextValue_);
}

ParameterList::ParameterList(const ParameterList& other) :
        parameters_(other.parameters_),
        parameterValues_(other.parameterValues_),
        parameterValueStrings_(other.parameterValueStrings_),
        onParameterChanged_(other.onParameterChanged_),
        onGetParameterValueString_(other.onGetParameterValueString_),
        onContextChanged_(other.onContextChanged_),
        context_(other.context_),
        contextValue_(other.contextValue_),
        perNoteKeyboard_(other.perNoteKeyboard_),
        contextValueLabels_(other.contextValueLabels_) {
    std::copy(std::begin(other.parameterFilter_), std::end(other.parameterFilter_), std::begin(parameterFilter_));
    bindKeyboardCallback();
    perNoteKeyboard_.setHighlightedKey(contextValue_);
}

ParameterList& ParameterList::operator=(const ParameterList& other) {
    if (this == &other)
        return *this;
    parameters_ = other.parameters_;
    parameterValues_ = other.parameterValues_;
    parameterValueStrings_ = other.parameterValueStrings_;
    std::copy(std::begin(other.parameterFilter_), std::end(other.parameterFilter_), std::begin(parameterFilter_));
    onParameterChanged_ = other.onParameterChanged_;
    onGetParameterValueString_ = other.onGetParameterValueString_;
    onContextChanged_ = other.onContextChanged_;
    context_ = other.context_;
    contextValue_ = other.contextValue_;
    perNoteKeyboard_ = other.perNoteKeyboard_;
    contextValueLabels_ = other.contextValueLabels_;
    bindKeyboardCallback();
    perNoteKeyboard_.setHighlightedKey(contextValue_);
    return *this;
}

ParameterList::ParameterList(ParameterList&& other) noexcept :
        parameters_(std::move(other.parameters_)),
        parameterValues_(std::move(other.parameterValues_)),
        parameterValueStrings_(std::move(other.parameterValueStrings_)),
        onParameterChanged_(std::move(other.onParameterChanged_)),
        onGetParameterValueString_(std::move(other.onGetParameterValueString_)),
        onContextChanged_(std::move(other.onContextChanged_)),
        context_(other.context_),
        contextValue_(other.contextValue_),
        perNoteKeyboard_(std::move(other.perNoteKeyboard_)),
        contextValueLabels_(std::move(other.contextValueLabels_)) {
    std::copy(std::begin(other.parameterFilter_), std::end(other.parameterFilter_), std::begin(parameterFilter_));
    bindKeyboardCallback();
    perNoteKeyboard_.setHighlightedKey(contextValue_);
}

ParameterList& ParameterList::operator=(ParameterList&& other) noexcept {
    if (this == &other)
        return *this;
    parameters_ = std::move(other.parameters_);
    parameterValues_ = std::move(other.parameterValues_);
    parameterValueStrings_ = std::move(other.parameterValueStrings_);
    std::copy(std::begin(other.parameterFilter_), std::end(other.parameterFilter_), std::begin(parameterFilter_));
    onParameterChanged_ = std::move(other.onParameterChanged_);
    onGetParameterValueString_ = std::move(other.onGetParameterValueString_);
    onContextChanged_ = std::move(other.onContextChanged_);
    context_ = other.context_;
    contextValue_ = other.contextValue_;
    perNoteKeyboard_ = std::move(other.perNoteKeyboard_);
    contextValueLabels_ = std::move(other.contextValueLabels_);
    bindKeyboardCallback();
    return *this;
}

void ParameterList::setParameters(const std::vector<ParameterMetadata>& parameters) {
    parameters_ = parameters;
    parameterValues_.resize(parameters_.size());
    parameterValueStrings_.resize(parameters_.size());
}

void ParameterList::setParameterValue(size_t index, float value) {
    if (index < parameterValues_.size()) {
        parameterValues_[index] = value;
    }
}

void ParameterList::setParameterValueString(size_t index, const std::string& valueString) {
    if (index < parameterValueStrings_.size()) {
        parameterValueStrings_[index] = valueString;
    }
}

void ParameterList::render() {
    static const char* kContextLabels[] = {"Global", "Group", "Channel", "Key"};

    if (ImGui::BeginTable("ParameterContextRow", 2,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("ContextControls", ImGuiTableColumnFlags_WidthFixed, 360.0f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(1);
        ImGui::PushID("PerNoteSelector");
        perNoteKeyboard_.render();
        ImGui::PopID();

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Context:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        int contextIndex = static_cast<int>(context_);
        if (ImGui::Combo("##ParameterContext", &contextIndex, kContextLabels,
                         IM_ARRAYSIZE(kContextLabels))) {
            contextIndex = std::clamp(contextIndex, 0, IM_ARRAYSIZE(kContextLabels) - 1);
            setContext(static_cast<ParameterContext>(contextIndex));
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("Value/Key:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        const int contextValueCount = static_cast<int>(contextValueLabels_.size());
        const auto& previewLabel = contextValueLabels_[static_cast<size_t>(contextValue_)];
        if (ImGui::BeginCombo("##ParameterContextValue", previewLabel.c_str())) {
            for (int idx = 0; idx < contextValueCount; ++idx) {
                bool isSelected = (idx == contextValue_);
                if (ImGui::Selectable(contextValueLabels_[static_cast<size_t>(idx)].c_str(), isSelected)) {
                    setContextValue(static_cast<uint8_t>(idx));
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Filter Parameters:");
    ImGui::SameLine();
    const ImVec4 filterBg(0.22f, 0.22f, 0.22f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, filterBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, filterBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, filterBg);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##ParameterFilter", parameterFilter_, sizeof(parameterFilter_));
    ImGui::PopStyleColor(3);

    const ImGuiTableFlags parameterTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate |
                                                ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("ParameterTable", 6, parameterTableFlags)) {
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                               ImGuiTableColumnFlags_PreferSortAscending,
                                30.0f);
        ImGui::TableSetupColumn("Stable ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        std::vector<size_t> visibleParameterIndices = filterAndBuildIndices();
        sortIndices(visibleParameterIndices);

        for (size_t paramIndex : visibleParameterIndices) {
            auto& param = parameters_[paramIndex];

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (param.path.empty()) {
                ImGui::TextUnformatted("-");
            } else {
                ImGui::TextUnformatted(param.path.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::Text("%u", param.index);

            ImGui::TableNextColumn();
            if (param.stableId.empty()) {
                ImGui::Text("(empty)");
            } else {
                ImGui::Text("%s", param.stableId.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", param.name.c_str());

            ImGui::TableNextColumn();
            std::string controlId = "##" + std::to_string(param.index);

            bool parameterChanged = false;
            const bool hasDiscreteCombo = !param.namedValues.empty();
            const char* format = parameterValueStrings_[paramIndex].empty()
                                     ? "%.3f"
                                     : parameterValueStrings_[paramIndex].c_str();

            float sliderWidth = ImGui::GetContentRegionAvail().x;
            float comboButtonWidth = 0.0f;
            float comboSpacing = 0.0f;
            if (hasDiscreteCombo) {
                comboButtonWidth = ImGui::GetFrameHeight();
                comboSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
                sliderWidth = std::max(20.0f, sliderWidth - (comboButtonWidth + comboSpacing));
            }

            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderFloat(controlId.c_str(), &parameterValues_[paramIndex],
                                   static_cast<float>(param.minPlainValue),
                                   static_cast<float>(param.maxPlainValue), format)) {
                parameterChanged = true;
            }

            ImVec2 sliderMin = ImGui::GetItemRectMin();
            ImVec2 sliderMax = ImGui::GetItemRectMax();

            if (hasDiscreteCombo) {
                ImGui::SameLine(0.0f, comboSpacing);
                std::string comboButtonId = controlId + "_combo";
                std::string comboPopupId = controlId + "_popup";
                const bool popupOpen = ImGui::IsPopupOpen(comboPopupId.c_str(), ImGuiPopupFlags_None);
                bool requestPopupClose = false;

                if (popupOpen) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                }

                if (ImGui::ArrowButton(comboButtonId.c_str(), ImGuiDir_Down)) {
                    if (popupOpen) {
                        requestPopupClose = true;
                    } else {
                        ImGui::OpenPopup(comboPopupId.c_str());
                    }
                }

                if (popupOpen) {
                    ImGui::PopStyleColor(2);
                }

                ImGui::SetNextWindowPos(sliderMin);
                ImGui::SetNextWindowSize(ImVec2(sliderMax.x - sliderMin.x, 0.0f));
                if (ImGui::BeginPopup(comboPopupId.c_str())) {
                    if (requestPopupClose) {
                        ImGui::CloseCurrentPopup();
                    } else {
                        const std::string& currentLabel = parameterValueStrings_[paramIndex].empty()
                                                               ? std::to_string(parameterValues_[paramIndex])
                                                               : parameterValueStrings_[paramIndex];

                        if (!currentLabel.empty()) {
                            ImGui::TextUnformatted(currentLabel.c_str());
                            ImGui::Separator();
                        }

                        for (const auto& namedValue : param.namedValues) {
                            bool isSelected = (std::abs(namedValue.value - parameterValues_[paramIndex]) < 0.0001);
                            if (ImGui::Selectable(namedValue.name.c_str(), isSelected)) {
                                parameterValues_[paramIndex] = static_cast<float>(namedValue.value);
                                parameterChanged = true;
                                ImGui::CloseCurrentPopup();
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }

            if (parameterChanged) {
                if (onParameterChanged_) {
                    onParameterChanged_(param.index, parameterValues_[paramIndex]);
                }
                if (onGetParameterValueString_) {
                    parameterValueStrings_[paramIndex] = onGetParameterValueString_(param.index, parameterValues_[paramIndex]);
                }
                std::cout << "Parameter " << param.name << " changed to " << parameterValues_[paramIndex] << std::endl;
            }

            ImGui::TableNextColumn();
            std::string resetId = "Reset##" + std::to_string(param.index);
            if (ImGui::Button(resetId.c_str())) {
                parameterValues_[paramIndex] = static_cast<float>(param.defaultPlainValue);
                if (onParameterChanged_) {
                    onParameterChanged_(param.index, parameterValues_[paramIndex]);
                }
                if (onGetParameterValueString_) {
                    parameterValueStrings_[paramIndex] = onGetParameterValueString_(param.index, parameterValues_[paramIndex]);
                }
            }
        }

        ImGui::EndTable();
    }
}

void ParameterList::setOnParameterChanged(ParameterChangeCallback callback) {
    onParameterChanged_ = callback;
}

void ParameterList::setOnGetParameterValueString(GetParameterValueStringCallback callback) {
    onGetParameterValueString_ = callback;
}

void ParameterList::setOnContextChanged(ContextChangeCallback callback) {
    onContextChanged_ = callback;
}

const std::vector<ParameterMetadata>& ParameterList::getParameters() const {
    return parameters_;
}

const std::vector<float>& ParameterList::getParameterValues() const {
    return parameterValues_;
}

float ParameterList::getParameterValue(size_t index) const {
    if (index < parameterValues_.size()) {
        return parameterValues_[index];
    }
    return 0.0f;
}

ParameterList::ParameterContext ParameterList::context() const {
    return context_;
}

uint8_t ParameterList::contextValue() const {
    return static_cast<uint8_t>(contextValue_);
}

void ParameterList::setContext(ParameterContext context) {
    if (context_ == context) {
        return;
    }
    context_ = context;
    perNoteKeyboard_.setHighlightedKey(contextValue_);
    notifyContextChanged();
}

void ParameterList::setContextValue(uint8_t value) {
    const int newValue = std::clamp(static_cast<int>(value), 0, 127);
    if (contextValue_ == newValue) {
        return;
    }
    contextValue_ = newValue;
    perNoteKeyboard_.setHighlightedKey(contextValue_);
    notifyContextChanged();
}

void ParameterList::clearFilter() {
    std::fill(std::begin(parameterFilter_), std::end(parameterFilter_), '\0');
}

const char* ParameterList::getFilter() const {
    return parameterFilter_;
}

std::vector<size_t> ParameterList::filterAndBuildIndices() {
    std::string filter = parameterFilter_;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    std::vector<size_t> visibleParameterIndices;
    visibleParameterIndices.reserve(parameters_.size());

    for (size_t i = 0; i < parameters_.size(); ++i) {
        auto& param = parameters_[i];

        if (param.hidden || (!param.automatable && !param.discrete))
            continue;

        if (!filter.empty()) {
            std::string name = param.name;
            std::string stableId = param.stableId;
            std::string path = param.path;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            std::transform(stableId.begin(), stableId.end(), stableId.begin(), ::tolower);
            std::transform(path.begin(), path.end(), path.begin(), ::tolower);
            if (name.find(filter) == std::string::npos && stableId.find(filter) == std::string::npos &&
                path.find(filter) == std::string::npos) {
                continue;
            }
        }

        visibleParameterIndices.push_back(i);
    }

    return visibleParameterIndices;
}

void ParameterList::sortIndices(std::vector<size_t>& indices) {
    auto compareByColumn = [&](size_t lhs, size_t rhs, int columnIndex) {
        const auto& leftParam = parameters_[lhs];
        const auto& rightParam = parameters_[rhs];
        switch (columnIndex) {
        case 0:
            if (leftParam.path < rightParam.path)
                return -1;
            if (leftParam.path > rightParam.path)
                return 1;
            return 0;
        case 1:
            if (leftParam.index < rightParam.index)
                return -1;
            if (leftParam.index > rightParam.index)
                return 1;
            return 0;
        case 2:
            if (leftParam.stableId < rightParam.stableId)
                return -1;
            if (leftParam.stableId > rightParam.stableId)
                return 1;
            return 0;
        case 3:
            if (leftParam.name < rightParam.name)
                return -1;
            if (leftParam.name > rightParam.name)
                return 1;
            return 0;
        case 4:
            if (parameterValues_[lhs] < parameterValues_[rhs])
                return -1;
            if (parameterValues_[lhs] > parameterValues_[rhs])
                return 1;
            return 0;
        case 5:
            if (leftParam.defaultPlainValue < rightParam.defaultPlainValue)
                return -1;
            if (leftParam.defaultPlainValue > rightParam.defaultPlainValue)
                return 1;
            return 0;
        default:
            return 0;
        }
    };

    if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
        if (!indices.empty() && sortSpecs->SpecsCount > 0) {
            std::stable_sort(indices.begin(), indices.end(),
                             [&](size_t lhs, size_t rhs) {
                                 for (int n = 0; n < sortSpecs->SpecsCount; ++n) {
                                     const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[n];
                                     int delta = compareByColumn(lhs, rhs, spec.ColumnIndex);
                                     if (delta != 0) {
                                         return spec.SortDirection == ImGuiSortDirection_Ascending ? (delta < 0)
                                                                                                   : (delta > 0);
                                     }
                                 }
                                 return lhs < rhs;
                             });
        }
        sortSpecs->SpecsDirty = false;
    }
}

void ParameterList::notifyContextChanged() {
    if (onContextChanged_) {
        onContextChanged_(context_, static_cast<uint8_t>(contextValue_));
    }
}

void ParameterList::bindKeyboardCallback() {
    perNoteKeyboard_.setKeyEventCallback([this](int note, int /*velocity*/, bool isPressed) {
        if (isPressed)
            setContextValue(static_cast<uint8_t>(std::clamp(note, 0, 127)));
    });
}

}
