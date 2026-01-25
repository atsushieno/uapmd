
#include <iostream>
#include <midicci/midicci.hpp>
#include "uapmd/uapmd.hpp"

namespace uapmd {
    void UapmdMidiCISessions::interceptUmpInput(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {

        for (auto& forwarder : ci_input_forwarders)
            forwarder(reinterpret_cast<uint8_t*>(static_cast<void*>(ump)), 0, sizeInBytes, timestamp);

    }

    void setupParameterList(std::string controlType,
                            std::vector<commonproperties::MidiCIControl>& allCtrlList,
                            std::vector<uapmd::ParameterMetadata>& parameterList,
                            MidiCIDevice& ciDevice) {
        for (auto& p : parameterList) {
            if (p.hidden || !p.automatable)
                continue;

            commonproperties::MidiCIControl ctrl{p.name, controlType, "",
                                                 std::vector<uint8_t>{static_cast<uint8_t>(p.index / 0x80), static_cast<uint8_t>(p.index % 0x80)}};
            ctrl.paramPath = p.path;
            const double range = p.maxPlainValue - p.minPlainValue;
            auto plainToUint32 = [&](double plainValue) -> uint32_t {
                if (!(range > 0.0))
                    return 0;
                double normalized = (plainValue - p.minPlainValue) / range;
                normalized = std::clamp(normalized, 0.0, 1.0);
                return static_cast<uint32_t>(normalized * static_cast<double>(UINT32_MAX));
            };
            if (p.defaultPlainValue != 0.0)
                ctrl.defaultValue = plainToUint32(p.defaultPlainValue);
            if (p.minPlainValue != 0.0 || p.maxPlainValue != 1.0)
                ctrl.minMax = {plainToUint32(p.minPlainValue), plainToUint32(p.maxPlainValue)};

            if (!p.namedValues.empty()) {
                ctrl.ctrlMapId = std::to_string(p.index);

                std::vector<MidiCIControlMap> ctrlMapList{};
                ctrlMapList.reserve(p.namedValues.size());
                for (auto &m: p.namedValues)
                    ctrlMapList.emplace_back(MidiCIControlMap{plainToUint32(m.value), m.name});
                StandardPropertiesExtensions::setCtrlMapList(ciDevice, std::to_string(p.index), ctrlMapList);
            }
            allCtrlList.push_back(ctrl);
        }
    }

    void UapmdMidiCISessions::setupMidiCISession() {
        auto instance_id = device->instanceId();

        midicci::MidiCIDeviceConfiguration ci_config{
            midicci::DEFAULT_RECEIVABLE_MAX_SYSEX_SIZE,
            midicci::DEFAULT_MAX_PROPERTY_CHUNK_SIZE,
            device_name,
            0
        };
        ci_config.device_info.manufacturer = manufacturer;
        ci_config.device_info.model = device_name;
        ci_config.device_info.version = version;

        auto device_info = ci_config.device_info;

        uint32_t muid{static_cast<uint32_t>(rand() & 0x7F7F7F7F)};

        auto input_listener_adder = [&](midicci::musicdevice::MidiInputCallback callback) {
            ci_input_forwarders.push_back(std::move(callback));
        };
        auto sender = [&](const uint8_t* data, size_t offset, size_t length, uint64_t timestamp) {
            if (!device->midiIO())
                return;
            device->midiIO()->send(const_cast<uapmd_ump_t*>(reinterpret_cast<const uapmd_ump_t*>(data + offset)),
                                 length,
                                 static_cast<uapmd_timestamp_t>(timestamp));
        };

        midicci::musicdevice::MidiCISessionSource source{
            midicci::musicdevice::MidiTransportProtocol::UMP,
            input_listener_adder,
            sender
        };

        auto ciSession = createMidiCiSession(source, muid, std::move(ci_config), [&](const LogData& log) {
            auto msg = std::get_if<std::reference_wrapper<const Message>>(&log.data);
            if (msg)
                std::cerr << "[UAPMD LOG " << (log.is_outgoing ? "OUT] " : "IN] ") << msg->get().getLogMessage() << std::endl;
            else
                std::cerr << "[UAPMD LOG " << (log.is_outgoing ? "OUT] " : "IN] ") << std::get<std::string>(log.data) << std::endl;
        });

        auto& ciDevice = ciSession->getDevice();

        auto& hostProps = ciDevice.getPropertyHostFacade();
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::allCtrlListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::chCtrlListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::ctrlMapListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::programListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::stateListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::stateMetadata()));

        hostProps.updateCommonRulesDeviceInfo(device_info);

        auto* instance = sequencer->getPluginInstance(instance_id);

        std::vector<commonproperties::MidiCIControl> allCtrlList{};

        auto parameterList = instance->parameterMetadataList();
        setupParameterList(MidiCIControlType::NRPN, allCtrlList, parameterList, ciDevice);
        // FIXME: we need some way to indicate the context key (it is impossible so far, by design).
        auto perNoteControllerList = instance->perNoteControllerMetadataList(remidy::PER_NOTE_CONTROLLER_PER_NOTE, 64);
        setupParameterList(MidiCIControlType::PNAC, allCtrlList, perNoteControllerList, ciDevice);

        StandardPropertiesExtensions::setAllCtrlList(ciDevice, allCtrlList);

        std::vector<uapmd::PresetsMetadata> presetsList = instance->presetMetadataList();
        std::vector<commonproperties::MidiCIProgram> programList{};
        programList.reserve(presetsList.size());
        for (auto& p : presetsList) {
            commonproperties::MidiCIProgram program{p.name,
                {static_cast<uint8_t>(p.bank / 0x80), static_cast<uint8_t>(p.bank % 0x80), static_cast<uint8_t>(p.index)}};
            programList.push_back(program);
        }
        StandardPropertiesExtensions::setProgramList(ciDevice, programList);

        // Set up custom property getter/setter for dynamic state management
        // Retrieve the original getter before setting the new one
        auto originalGetter = hostProps.getPropertyBinaryGetter();

        // Create custom property getter that uses AudioPluginNode::saveState() for State/fullState
        auto customGetter = [this, originalGetter](const std::string& property_id, const std::string& res_id) -> std::vector<uint8_t> {
            if (property_id == StandardPropertyNames::STATE && res_id == MidiCIStatePredefinedNames::FULL_STATE) {
                if (sequencer && device->instanceId() >= 0) {
                    auto* instance = sequencer->getPluginInstance(device->instanceId());
                    if (instance) {
                        return instance->saveState();
                    }
                }
                return {};
            }
            // Fall back to the original delegate
            return originalGetter(property_id, res_id);
        };
        hostProps.setPropertyBinaryGetter(customGetter);

        // Retrieve the original setter before setting the new one
        auto originalSetter = hostProps.getPropertyBinarySetter();

        // Create custom property setter that uses AudioPluginNode::loadState() for State/fullState
        auto customSetter = [this, originalSetter](const std::string& property_id, const std::string& res_id,
                                                     const std::string& media_type, const std::vector<uint8_t>& body) -> bool {
            if (property_id == StandardPropertyNames::STATE && res_id == MidiCIStatePredefinedNames::FULL_STATE) {
                if (sequencer && device->instanceId() >= 0) {
                    auto* instance = sequencer->getPluginInstance(device->instanceId());
                    if (instance) {
                        std::vector<uint8_t> state = body;
                        instance->loadState(state);
                        return true;
                    }
                }
                return false;
            }
            // Fall back to the original delegate
            return originalSetter(property_id, res_id, media_type, body);
        };
        hostProps.setPropertyBinarySetter(customSetter);

        ci_sessions[muid] = std::move(ciSession);

        if (sequencer) {
            sequencer->setPluginOutputHandler(instance_id, [this](const uapmd_ump_t* data, size_t bytes) {
                if (!device->midiIO())
                    return;
                device->midiIO()->send(const_cast<uapmd_ump_t*>(data), bytes, 0);
            });
        }
    }
}
