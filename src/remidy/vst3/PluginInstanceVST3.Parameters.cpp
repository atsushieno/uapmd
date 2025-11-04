
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

remidy::PluginInstanceVST3::ParameterSupport::ParameterSupport(PluginInstanceVST3* owner) : owner(owner) {
    auto controller = owner->controller;
    auto count = controller->getParameterCount();

    parameter_ids = (ParamID*) calloc(sizeof(ParamID), count);

    for (auto i = 0; i < count; i++) {
        ParameterInfo info{};
        controller->getParameterInfo(i, info);
        std::string idString = std::format("{}", info.id);
        std::string name = vst3StringToStdString(info.title);
        std::string path{""};

        std::vector<ParameterEnumeration> enums{};
        // VST3 stepCount is the maximum value, so stepCount+1 discrete values exist (0 to stepCount)
        if (info.stepCount > 0)
            for (int32_t e = 0; e <= info.stepCount; e++) {
                String128 nameStr{};
                auto normalized = e / (double) info.stepCount;
                controller->getParamStringByValue(info.id, normalized, nameStr);
                auto nameString = vst3StringToStdString(nameStr);
                ParameterEnumeration p{nameString, normalized};
                enums.emplace_back(p);
            }

        auto p = new PluginParameter(i, idString, name, path,
                                     info.defaultNormalizedValue,
                                     0.0,
                                     1.0,
                                     info.stepCount > 0 || (info.flags & ParameterInfo::kCanAutomate),
                                     true, // I don't see any flags for `readable`
                                     info.flags & ParameterInfo::kIsHidden,
                                     info.flags & ParameterInfo::kIsList,
                                     enums);

        parameter_ids[i] = info.id;
        parameter_defs.emplace_back(p);
    }
}

remidy::PluginInstanceVST3::ParameterSupport::~ParameterSupport() {
    for (auto p : parameter_defs)
        delete p;
    for (auto pair : per_note_controller_defs)
        for (auto p : pair.second)
            delete p;
    free(parameter_ids);
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceVST3::ParameterSupport::parameters() {
    return parameter_defs;
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceVST3::ParameterSupport::perNoteControllers(
    PerNoteControllerContextTypes types,
    PerNoteControllerContext context
) {
    for (auto& p : per_note_controller_defs)
        if (p.first.group == context.group && p.first.channel == context.channel)
            return p.second;
    std::vector<PluginParameter*> defs{};
    // populate the PNC list only if INoteExpressionController is implemented...
    auto nec = owner->note_expression_controller;
    if (nec) {
        auto count = nec->getNoteExpressionCount(context.group, context.channel);
        for (auto i = 0; i < count; i++) {
            NoteExpressionTypeInfo info{};
            if (nec->getNoteExpressionInfo(context.group, context.channel, i, info) == kResultOk) {
                auto name = vst3StringToStdString(info.shortTitle);
                static std::string empty{};
                auto parameter = new PluginParameter(i, name, name, empty,
                    info.valueDesc.defaultValue,
                    info.valueDesc.minimum,
                    info.valueDesc.maximum,
                    true, true, false, false);
                defs.emplace_back(parameter);
            }
        }
    }
    const auto ctx = PerNoteControllerContext{0, context.channel, context.group, 0};
    per_note_controller_defs.emplace_back(ctx, defs);
    return perNoteControllers(types, ctx);
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // use IParameterChanges.
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    auto pvc = owner->processDataInputParameterChanges.asInterface();
    const ParamID id = parameter_ids[index];
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
    if (q && q->addPoint(sampleOffset, value, i) == kResultOk)
        return StatusCode::OK;
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
    evt.noteExpressionValue.typeId = index;
    evt.noteExpressionValue.noteId = static_cast<int32_t>(context.note);
    evt.noteExpressionValue.value = value;
    auto evtList = owner->processDataOutputEvents.asInterface();
    // This should copy the argument evt, right?
    evtList->addEvent(evt);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto controller = owner->controller;
    *value = controller->getParamNormalized(parameter_ids[index]);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) {
    return StatusCode::NOT_IMPLEMENTED;
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
    auto enums = parameter_defs[index]->enums();
    if (enums.empty())
        return "";
    if (value == 1.0)
        return enums[enums.size() - 1].label;
    return enums[(int32_t) (value * enums.size())].label;
}
