
#include <iostream>
#include <memory>
#include <optional>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

remidy::PluginInstanceVST3::ParameterSupport::ParameterSupport(PluginInstanceVST3* owner) : owner(owner) {
    populateParameterList();
}

void remidy::PluginInstanceVST3::ParameterSupport::clearParameterList() {
    for (auto* param : parameter_defs)
        delete param;
    parameter_defs.clear();
    parameter_ids.clear();
    param_id_to_index.clear();
    program_change_parameter_id = static_cast<ParamID>(-1);
    program_change_parameter_index = -1;
}

void remidy::PluginInstanceVST3::ParameterSupport::populateParameterList() {
    clearParameterList();

    auto controller = owner->controller;
    if (!controller)
        return;

    const int32 count = controller->getParameterCount();
    if (count <= 0)
        return;

    parameter_ids.assign(static_cast<size_t>(count), 0);
    parameter_defs.reserve(static_cast<size_t>(count));
    param_id_to_index.reserve(static_cast<size_t>(count));

    buildUnitHierarchy();

    for (int32 i = 0; i < count; i++) {
        ParameterInfo info{};
        controller->getParameterInfo(i, info);
        std::string idString = std::format("{}", info.id);
        std::string name = vst3StringToStdString(info.title);
        std::string path = buildUnitPath(info.unitId);

        // Query plain (denormalized) value ranges from VST3
        double plainMin = controller->normalizedParamToPlain(info.id, 0.0);
        double plainMax = controller->normalizedParamToPlain(info.id, 1.0);
        double plainDefault = controller->normalizedParamToPlain(info.id, info.defaultNormalizedValue);

        std::vector<ParameterEnumeration> enums{};
        // VST3 stepCount is the maximum value, so stepCount+1 discrete values exist (0 to stepCount)
        if (info.stepCount > 0)
            for (int32_t e = 0; e <= info.stepCount; e++) {
                String128 nameStr{};
                auto normalized = e / (double) info.stepCount;
                auto plainValue = controller->normalizedParamToPlain(info.id, normalized);
                controller->getParamStringByValue(info.id, normalized, nameStr);
                auto nameString = vst3StringToStdString(nameStr);
                ParameterEnumeration p{nameString, plainValue};
                enums.emplace_back(p);
            }

        auto p = new PluginParameter(static_cast<uint32_t>(i), idString, name, path,
                                     plainDefault,
                                     plainMin,
                                     plainMax,
                                     info.stepCount > 0 || (info.flags & ParameterInfo::kCanAutomate),
                                     true, // I don't see any flags for `readable`
                                     info.flags & ParameterInfo::kIsHidden,
                                     info.flags & ParameterInfo::kIsList,
                                     enums);

        parameter_ids[static_cast<size_t>(i)] = info.id;
        parameter_defs.emplace_back(p);
        param_id_to_index[info.id] = static_cast<uint32_t>(i);

        // Detect program change parameter (marked with kIsProgramChange flag)
        if (info.flags & ParameterInfo::kIsProgramChange) {
            program_change_parameter_id = info.id;
            program_change_parameter_index = static_cast<int32_t>(i);
        }
    }
}

void remidy::PluginInstanceVST3::ParameterSupport::rebuildParameterList() {
    populateParameterList();
}

void remidy::PluginInstanceVST3::ParameterSupport::refreshAllParameterMetadata() {
    rebuildParameterList();
    broadcastAllParameterValues();
    parameterMetadataChangeEvent().notify();
}

void remidy::PluginInstanceVST3::ParameterSupport::broadcastAllParameterValues() {
    for (size_t i = 0; i < parameter_defs.size(); ++i) {
        double value{};
        if (getParameter(static_cast<uint32_t>(i), &value) == StatusCode::OK)
            parameterChangeEvent().notify(i, value);
    }
}

remidy::PluginInstanceVST3::ParameterSupport::~ParameterSupport() {
    clearParameterList();
    per_note_controller_defs.clear();
    per_note_controller_storage.clear();
    note_expression_info_map.clear();
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceVST3::ParameterSupport::parameters() {
    return parameter_defs;
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceVST3::ParameterSupport::perNoteControllers(
    PerNoteControllerContextTypes types,
    PerNoteControllerContext context
) {
    (void) types;
    per_note_controller_defs.clear();
    per_note_controller_storage.clear();
    clearNoteExpressionInfo(context.group, context.channel);

    // populate the PNC list only if INoteExpressionController is implemented...
    auto nec = owner->note_expression_controller;
    if (nec) {
        const auto count = nec->getNoteExpressionCount(context.group, context.channel);
        per_note_controller_defs.reserve(static_cast<size_t>(count));
        per_note_controller_storage.reserve(static_cast<size_t>(count));
        for (auto i = 0; i < count; i++) {
            NoteExpressionTypeInfo info{};
            if (nec->getNoteExpressionInfo(context.group, context.channel, i, info) == kResultOk) {
                auto name = vst3StringToStdString(info.shortTitle);
                static std::string empty{};
                auto parameter = std::make_unique<PluginParameter>(i, name, name, empty,
                    info.valueDesc.defaultValue,
                    info.valueDesc.minimum,
                    info.valueDesc.maximum,
                    true, true, false, false);
                NoteExpressionKey key{context.group, context.channel, static_cast<uint32_t>(i)};
                note_expression_info_map[key] = info;
                per_note_controller_defs.emplace_back(parameter.get());
                per_note_controller_storage.emplace_back(std::move(parameter));
            }
        }
    }
    return per_note_controller_defs;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // use IParameterChanges.
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    auto pvc = owner->processDataInputParameterChanges.asInterface();
    if (index >= parameter_ids.size())
        return StatusCode::INVALID_PARAMETER_OPERATION;
    const ParamID id = parameter_ids[index];

    // Convert plain value to normalized for VST3
    auto normalized = owner->controller->plainParamToNormalized(id, value);

    int32_t i = 0;
    IParamValueQueue* q{nullptr};
    for (int32_t n = pvc->getParameterCount(); i < n; i++) {
        q = pvc->getParameterData(i);
        if (q->getParameterId() == id)
            break;
    }
    if (!q)
        // It is an RT-safe operation, right?
        q = pvc->addParameterData(id, i);
    if (q && q->addPoint(sampleOffset, normalized, i) == kResultOk) {
        // Notify parameter change event listeners (e.g., UMP output mapper)
        parameterChangeEvent().notify(index, value);
        return StatusCode::OK;
    }
    return StatusCode::INVALID_PARAMETER_OPERATION;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    double ppqPosition = owner->ump_input_dispatcher.trackContext()->ppqPosition(); // I guess only either of those time options are needed.
    uint16_t flags = owner->processData.processMode == kRealtime ? Event::kIsLive : 0; // am I right?
    Event evt{};
    evt.busIndex = static_cast<int32_t>(context.group);
    evt.sampleOffset = sampleOffset;
    evt.ppqPosition = ppqPosition;
    evt.flags = flags;
    evt.type = Event::kNoteExpressionValueEvent;
    NoteExpressionTypeID typeId = static_cast<NoteExpressionTypeID>(index);
    if (const auto* info = resolveNoteExpressionInfo(context.group, context.channel, index))
        typeId = info->typeId;
    evt.noteExpressionValue.typeId = typeId;
    evt.noteExpressionValue.noteId = static_cast<int32_t>(context.note);
    evt.noteExpressionValue.value = value;
    auto evtList = owner->processDataOutputEvents.asInterface();
    // This should copy the argument evt, right?
    evtList->addEvent(evt);

    // Notify per-note controller change event listeners (e.g., UMP output mapper)
    perNoteControllerChangeEvent().notify(PER_NOTE_CONTROLLER_PER_NOTE, context.note, index, value);

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto controller = owner->controller;
    if (!controller || index >= parameter_ids.size())
        return StatusCode::INVALID_PARAMETER_OPERATION;
    const ParamID id = parameter_ids[index];

    // Get normalized value from VST3 and convert to plain
    auto normalized = controller->getParamNormalized(id);
    *value = controller->normalizedParamToPlain(id, normalized);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) {
    return StatusCode::NOT_IMPLEMENTED;
}

void remidy::PluginInstanceVST3::ParameterSupport::buildUnitHierarchy() {
    if (!unit_hierarchy.empty() || owner->unit_info == nullptr)
        return;

    auto unitInfo = owner->unit_info;
    const int32 unitCount = unitInfo->getUnitCount();
    for (int32 i = 0; i < unitCount; ++i) {
        UnitInfo info{};
        if (unitInfo->getUnitInfo(i, info) != kResultOk)
            continue;
        std::string unitName = vst3StringToStdString(info.name);
        unit_hierarchy[info.id] = std::make_pair(info.parentUnitId, unitName);
    }
}

std::string remidy::PluginInstanceVST3::ParameterSupport::buildUnitPath(UnitID unitId) {
    if (owner->unit_info == nullptr)
        return {};
    if (unitId == Vst::kRootUnitId)
        return {};

    auto cached = unit_path_cache.find(unitId);
    if (cached != unit_path_cache.end())
        return cached->second;

    auto it = unit_hierarchy.find(unitId);
    if (it == unit_hierarchy.end())
        return {};

    const std::string& name = it->second.second;
    UnitID parentId = it->second.first;

    std::string parentPath;
    if (parentId != unitId && parentId != Vst::kRootUnitId)
        parentPath = buildUnitPath(parentId);

    std::string path;
    if (!parentPath.empty() && !name.empty())
        path = parentPath + "/" + name;
    else if (!parentPath.empty())
        path = parentPath;
    else
        path = name;

    unit_path_cache[unitId] = path;
    return path;
}

void remidy::PluginInstanceVST3::ParameterSupport::setProgramChange(remidy::uint4_t group, remidy::uint4_t channel,
                                                                    remidy::uint7_t flags, remidy::uint7_t program,
                                                                    remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    auto unitInfo = owner->unit_info;
    auto states = owner->_states;

    int32_t bank = (bankMSB << 7) + bankLSB;

    ProgramListInfo pl;
    auto result = unitInfo->getProgramListInfo(bank, pl);
    if (result != kResultOk) {
        std::cerr << std::format("Could not retrieve program list: result code: {}, bank: {}, program: {}", result, bank, program) << std::endl;
        return; // could not retrieve program list
    }

    IBStream *stream;
    result = unitInfo->setUnitProgramData(pl.id, program, stream);
    if (result != kResultOk) {
        std::cerr << std::format("Failed to set unit program data: result code: {}, bank: {}, program: {}", result, bank, program) << std::endl;
        return;
    }

    int64_t size;
    stream->seek(0, IBStream::kIBSeekEnd, &size);
    std::vector<uint8_t> buf(size);
    int32_t read;
    stream->read(buf.data(), size, &read);
    states->setState(buf, remidy::PluginStateSupport::StateContextType::Preset, true);
}

std::string remidy::PluginInstanceVST3::ParameterSupport::valueToString(uint32_t index, double value) {
    auto controller = owner->controller;
    if (!controller || index >= parameter_ids.size())
        return "";
    const ParamID id = parameter_ids[index];

    // Convert plain value to normalized for VST3 API
    auto normalized = controller->plainParamToNormalized(id, value);

    // Use VST3's getParamStringByValue for formatted output
    String128 stringBuffer{};
    if (controller->getParamStringByValue(id, normalized, stringBuffer) == kResultOk) {
        return vst3StringToStdString(stringBuffer);
    }

    // Fallback to empty string if conversion fails
    return "";
}

std::string remidy::PluginInstanceVST3::ParameterSupport::valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) {
    auto nec = owner->note_expression_controller;
    if (!nec)
        return "";

    const auto* info = resolveNoteExpressionInfo(context.group, context.channel, index);
    if (!info)
        return "";

    double normalized = value;
    const double minValue = info->valueDesc.minimum;
    const double maxValue = info->valueDesc.maximum;
    if (maxValue > minValue)
        normalized = (value - minValue) / (maxValue - minValue);
    if (normalized < 0.0)
        normalized = 0.0;
    else if (normalized > 1.0)
        normalized = 1.0;

    String128 stringBuffer{};
    if (nec->getNoteExpressionStringByValue(context.group, context.channel, info->typeId, normalized, stringBuffer) == kResultOk)
        return vst3StringToStdString(stringBuffer);
    return "";
}

void remidy::PluginInstanceVST3::ParameterSupport::clearNoteExpressionInfo(uint32_t group, uint32_t channel) {
    for (auto it = note_expression_info_map.begin(); it != note_expression_info_map.end();) {
        if (it->first.group == group && it->first.channel == channel)
            it = note_expression_info_map.erase(it);
        else
            ++it;
    }
}

const NoteExpressionTypeInfo* remidy::PluginInstanceVST3::ParameterSupport::resolveNoteExpressionInfo(uint32_t group, uint32_t channel, uint32_t index) {
    NoteExpressionKey key{group, channel, index};
    auto it = note_expression_info_map.find(key);
    if (it != note_expression_info_map.end())
        return &it->second;

    auto nec = owner->note_expression_controller;
    if (!nec)
        return nullptr;

    NoteExpressionTypeInfo info{};
    if (nec->getNoteExpressionInfo(group, channel, static_cast<int32>(index), info) != kResultOk)
        return nullptr;

    auto [insertedIt, _] = note_expression_info_map.emplace(key, info);
    return &insertedIt->second;
}

void remidy::PluginInstanceVST3::ParameterSupport::refreshParameterMetadata(uint32_t index) {
    if (index >= parameter_defs.size() || index >= parameter_ids.size())
        return;

    auto paramId = parameter_ids[index];
    auto controller = owner->controller;
    if (!controller)
        return;

    double plainMin = controller->normalizedParamToPlain(paramId, 0.0);
    double plainMax = controller->normalizedParamToPlain(paramId, 1.0);

    ParameterInfo info{};
    controller->getParameterInfo(index, info);
    double plainDefault = controller->normalizedParamToPlain(paramId, info.defaultNormalizedValue);

    parameter_defs[index]->updateRange(plainMin, plainMax, plainDefault);
}

std::optional<uint32_t> remidy::PluginInstanceVST3::ParameterSupport::indexForParamId(ParamID id) const {
    auto it = param_id_to_index.find(id);
    if (it == param_id_to_index.end())
        return std::nullopt;
    return it->second;
}

void remidy::PluginInstanceVST3::ParameterSupport::notifyParameterValue(ParamID id, double plainValue) {
    if (auto index = indexForParamId(id); index.has_value())
        parameterChangeEvent().notify(index.value(), plainValue);
}

void remidy::PluginInstanceVST3::ParameterSupport::notifyPerNoteControllerValue(PerNoteControllerContextTypes contextType, uint32_t contextValue, uint32_t index, double plainValue) {
    perNoteControllerChangeEvent().notify(contextType, contextValue, index, plainValue);
}

std::optional<uint32_t> remidy::PluginInstanceVST3::ParameterSupport::indexForNoteExpressionType(uint32_t group, uint32_t channel, NoteExpressionTypeID typeId) {
    for (const auto& entry : note_expression_info_map) {
        if (entry.first.group == group && entry.first.channel == channel && entry.second.typeId == typeId)
            return entry.first.index;
    }

    auto nec = owner->note_expression_controller;
    if (!nec)
        return std::nullopt;

    const auto count = nec->getNoteExpressionCount(group, channel);
    if (count <= 0)
        return std::nullopt;

    for (int32 i = 0; i < count; ++i) {
        if (const auto* info = resolveNoteExpressionInfo(group, channel, static_cast<uint32_t>(i))) {
            if (info->typeId == typeId)
                return static_cast<uint32_t>(i);
        }
    }
    return std::nullopt;
}
