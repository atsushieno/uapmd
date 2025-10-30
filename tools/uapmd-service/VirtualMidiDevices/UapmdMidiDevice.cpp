#include "UapmdMidiDevice.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>

using namespace midicci::commonproperties;

namespace uapmd {

    UapmdMidiDevice::UapmdMidiDevice(AudioPluginSequencer* sharedSequencer,
                                     int32_t instanceId,
                                     int32_t trackIndex,
                                     std::string apiName,
                                     std::string deviceName,
                                     std::string manufacturerName,
                                     std::string versionString)
        : api_name(std::move(apiName)),
          device_name(std::move(deviceName)),
          manufacturer(std::move(manufacturerName)),
          version(std::move(versionString)),
          sequencer(sharedSequencer),
          instance_id(instanceId),
          track_index(trackIndex) {
    }

    UapmdMidiDevice::~UapmdMidiDevice() {
        stop();
        teardownOutputHandler();
    }

    void UapmdMidiDevice::teardownOutputHandler() {
        if (!sequencer || instance_id < 0)
            return;
        if (output_handler_registered) {
            sequencer->setPluginOutputHandler(instance_id, nullptr);
            output_handler_registered = false;
        }
    }

    void UapmdMidiDevice::setupMidiCISession() {
        if (!sequencer)
            return;

        midicci::MidiCIDeviceConfiguration ci_config{
            midicci::DEFAULT_RECEIVABLE_MAX_SYSEX_SIZE,
            midicci::DEFAULT_MAX_PROPERTY_CHUNK_SIZE,
            std::format("uapmd-service {}", instance_id),
            0
        };

        uint32_t muid{static_cast<uint32_t>(rand() & 0x7F7F7F7F)};

        auto input_listener_adder = [&](midicci::musicdevice::MidiInputCallback callback) {
            ci_input_forwarders.push_back(std::move(callback));
        };
        auto sender = [&](const uint8_t* data, size_t offset, size_t length, uint64_t timestamp) {
            if (!platformDevice)
                return;
            platformDevice->send(const_cast<uapmd_ump_t*>(reinterpret_cast<const uapmd_ump_t*>(data + offset)),
                                 length,
                                 static_cast<uapmd_timestamp_t>(timestamp));
        };

        midicci::musicdevice::MidiCISessionSource source{
            midicci::musicdevice::MidiTransportProtocol::UMP,
            input_listener_adder,
            sender
        };

        auto ciSession = create_midi_ci_session(source, muid, std::move(ci_config), [&](const LogData& log) {
            auto msg = std::get_if<std::reference_wrapper<const Message>>(&log.data);
            if (msg)
                std::cerr << "[UAPMD LOG " << (log.is_outgoing ? "OUT] " : "IN] ") << msg->get().get_log_message() << std::endl;
            else
                std::cerr << "[UAPMD LOG " << (log.is_outgoing ? "OUT] " : "IN] ") << std::get<std::string>(log.data) << std::endl;
        });

        auto& ciDevice = ciSession->get_device();

        auto& hostProps = ciDevice.get_property_host_facade();
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::allCtrlListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::chCtrlListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::ctrlMapListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::programListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::stateListMetadata()));
        hostProps.addMetadata(std::make_unique<CommonRulesPropertyMetadata>(StandardProperties::stateMetadata()));

        auto parameterList = sequencer->getParameterList(instance_id);
        std::vector<commonproperties::MidiCIControl> allCtrlList{};
        allCtrlList.reserve(parameterList.size());
        for (auto& p : parameterList) {
<<<<<<< HEAD
            if (p.hidden)
=======
            if (p.hidden || !p.automatable)
>>>>>>> d4d9055 (Add `automatable` field/function to plugin parameters.)
                continue;

            commonproperties::MidiCIControl ctrl{p.name, MidiCIControlType::NRPN, "",
                                                 std::vector<uint8_t>{static_cast<uint8_t>(p.index / 0x80), static_cast<uint8_t>(p.index % 0x80)}};
            ctrl.paramPath = p.path;
            ctrl.defaultValue = static_cast<uint32_t>(p.initialValue * UINT32_MAX);
            ctrl.minMax = {static_cast<uint32_t>(p.minValue * UINT32_MAX), static_cast<uint32_t>(p.maxValue * UINT32_MAX)};

            if (!p.namedValues.empty()) {
                ctrl.ctrlMapId = p.name;

                std::vector<MidiCIControlMap> ctrlMapList{};
                ctrlMapList.reserve(p.namedValues.size());
                for (auto &m: p.namedValues)
                    ctrlMapList.emplace_back(MidiCIControlMap{static_cast<uint32_t>(m.value * UINT32_MAX), m.name});
                StandardPropertiesExtensions::setCtrlMapList(ciDevice, p.name, ctrlMapList);
            }
            allCtrlList.push_back(ctrl);
        }
        StandardPropertiesExtensions::setAllCtrlList(ciDevice, allCtrlList);

        auto presetsList = sequencer->getPresetList(instance_id);
        std::vector<commonproperties::MidiCIProgram> programList{};
        programList.reserve(presetsList.size());
        for (auto& p : presetsList) {
            commonproperties::MidiCIProgram program{p.name,
                {static_cast<uint8_t>(p.bank / 0x80), static_cast<uint8_t>(p.bank % 0x80), static_cast<uint8_t>(p.index)}};
            programList.push_back(program);
        }
        StandardPropertiesExtensions::setProgramList(ciDevice, programList);

        ci_sessions[muid] = std::move(ciSession);

        if (!output_handler_registered && sequencer) {
            sequencer->setPluginOutputHandler(instance_id, [this](const uapmd_ump_t* data, size_t bytes) {
                if (!platformDevice)
                    return;
                platformDevice->send(const_cast<uapmd_ump_t*>(data), bytes, 0);
            });
            output_handler_registered = true;
        }
    }

    void UapmdMidiDevice::initialize() {
        if (sequencer) {
            if (auto groupOpt = sequencer->pluginGroup(instance_id); groupOpt.has_value()) {
                ump_group = groupOpt.value();
            }
        }
        setupMidiCISession();
    }

    uapmd_status_t UapmdMidiDevice::start() {
        if (!sequencer)
            return -1;

        platformDevice = std::make_unique<PlatformVirtualMidiDevice>(api_name, device_name, manufacturer, version);
        platformDevice->addInputHandler(&UapmdMidiDevice::umpReceivedTrampoline, this);

        return 0;
    }

    uapmd_status_t UapmdMidiDevice::stop() {
        if (platformDevice) {
            platformDevice->removeInputHandler(&UapmdMidiDevice::umpReceivedTrampoline);
            platformDevice.reset(nullptr);
        }
        return 0;
    }

    void UapmdMidiDevice::umpReceivedTrampoline(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        static_cast<UapmdMidiDevice*>(context)->umpReceived(ump, sizeInBytes, timestamp);
    }

    void UapmdMidiDevice::umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        if (!sequencer)
            return;

        for (auto& forwarder : ci_input_forwarders)
            forwarder(reinterpret_cast<uint8_t*>(static_cast<void*>(ump)), 0, sizeInBytes, timestamp);

        if (instance_id >= 0) {
            sequencer->enqueueUmpForInstance(instance_id, ump, sizeInBytes, timestamp);
            return;
        }

        auto groupId = static_cast<uint8_t>((ump[0] >> 28) & 0x0F);
        if (auto instance = sequencer->instanceForGroup(groupId); instance.has_value()) {
            sequencer->enqueueUmpForInstance(instance.value(), ump, sizeInBytes, timestamp);
        } else {
            sequencer->enqueueUmp(groupId, ump, sizeInBytes, timestamp);
        }
    }

}
