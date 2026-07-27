
#include "uapmd/uapmd.hpp"
#include "LibreMidiIODevice.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#include <libremidi/libremidi.hpp>
#include <remidy/detail/common.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
bool isWindowsMidiServicesSupported() {
    // Check for the MIDI 2.0 USB driver
    // If %SystemRoot%\System32\Drivers\usbmidi2.sys is missing,
    // the system cannot handle MIDI 2.0 hardware natively
    wchar_t systemRoot[MAX_PATH];
    if (GetEnvironmentVariableW(L"SystemRoot", systemRoot, MAX_PATH) == 0)
        return false;

    std::wstring driverPath = std::wstring(systemRoot) + L"\\SysWOW64\\wdmaud2.drv";

    DWORD attribs = GetFileAttributesW(driverPath.c_str());
    return (attribs != INVALID_FILE_ATTRIBUTES) && !(attribs & FILE_ATTRIBUTE_DIRECTORY);
}
#endif

bool matches_api_name(const std::string& value, const char* name) {
    if (value.empty() || name == nullptr)
        return false;
    return remidy_strcasecmp(value.c_str(), name) == 0;
}

std::optional<libremidi::API> select_if_available(const std::vector<libremidi::API>& apis, libremidi::API api) {
    if (std::find(apis.begin(), apis.end(), api) != apis.end())
        return api;
    return std::nullopt;
}

bool running_in_container() {
#if defined(__linux__)
    const char* flatpakId = std::getenv("FLATPAK_ID");
    if (flatpakId && *flatpakId)
        return true;
    std::error_code ec;
    if (std::filesystem::exists("/.flatpak-info", ec))
        return true;
    const char* snapPath = std::getenv("SNAP");
    if (snapPath && *snapPath)
        return true;
    const char* snapName = std::getenv("SNAP_NAME");
    if (snapName && *snapName)
        return true;
#endif
    return false;
}

std::optional<libremidi::API> pick_default_api(const std::vector<libremidi::API>& apis) {
    if (apis.empty())
        return std::nullopt;

    std::vector<libremidi::API> preferred;
    if (running_in_container()) {
        preferred.push_back(libremidi::API::PIPEWIRE_UMP);
    }
    preferred.insert(preferred.end(), {
        libremidi::API::ALSA_SEQ_UMP,
        libremidi::API::WINDOWS_MIDI_SERVICES,
        libremidi::API::JACK_UMP,
        libremidi::API::COREMIDI_UMP,
        libremidi::API::ALSA_RAW_UMP,
        libremidi::API::NETWORK_UMP,
        libremidi::API::KEYBOARD_UMP
    });

    for (auto candidate : preferred) {
        if (auto match = select_if_available(apis, candidate))
            return match;
    }

    return apis.front();
}

template <typename Port>
std::string portId(const Port& port) {
    // This is libremidi's port identity: it is API-local and opaque, but does
    // not depend on display metadata that may change when an endpoint opens.
    return std::format("{}:{}", static_cast<int>(port.api), port.port);
}

template <typename Port>
std::string displayName(const Port& port) {
    if (!port.display_name.empty())
        return port.display_name;
    if (!port.port_name.empty())
        return port.port_name;
    if (!port.device_name.empty())
        return port.device_name;
    return "Unnamed MIDI port";
}

template <typename Port>
bool isExcluded(const Port& port, const std::vector<std::string>& excluded) {
    return std::ranges::find(excluded, portId(port)) != excluded.end();
}

libremidi::observer makeObserver() {
    libremidi::observer_configuration configuration;
    configuration.track_hardware = true;
    configuration.track_virtual = true;
    configuration.track_network = true;
    configuration.track_any = true;
    return libremidi::observer(configuration);
}

class LibreMidiInputPort final : public uapmd::MidiIOFeature {
    std::vector<uapmd::ump_receiver_t> receivers_;
    std::vector<void*> receiver_user_data_;
    std::unique_ptr<libremidi::midi_in> midi_in_;

    void inputCallback(libremidi::ump&& message) {
        const auto sizeInBytes = umppi::Ump(message.data[0]).getSizeInBytes();
        for (size_t i = 0; i < receivers_.size(); ++i)
            receivers_[i](receiver_user_data_[i], const_cast<uint32_t*>(message.data), sizeInBytes, message.timestamp);
    }

public:
    explicit LibreMidiInputPort(const libremidi::input_port& port) {
        libremidi::ump_input_configuration configuration;
        configuration.on_message = [this](libremidi::ump&& message) { inputCallback(std::move(message)); };
        configuration.ignore_sysex = false;
        midi_in_ = std::make_unique<libremidi::midi_in>(configuration, port.api);
        if (auto error = midi_in_->open_port(port); error.is_set())
            throw std::runtime_error("Failed to open MIDI input port");
    }

    ~LibreMidiInputPort() override {
        if (midi_in_)
            midi_in_->close_port();
    }

    void addInputHandler(uapmd::ump_receiver_t receiver, void* userData) override {
        receivers_.push_back(receiver);
        receiver_user_data_.push_back(userData);
    }

    void removeInputHandler(uapmd::ump_receiver_t receiver) override {
        const auto position = std::find(receivers_.begin(), receivers_.end(), receiver);
        if (position == receivers_.end())
            return;
        const auto index = static_cast<size_t>(position - receivers_.begin());
        receivers_.erase(position);
        receiver_user_data_.erase(receiver_user_data_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void send(uapmd_ump_t*, size_t, uapmd_timestamp_t) override {}
};

class LibreMidiOutputPort final : public uapmd::MidiIOFeature {
    std::unique_ptr<libremidi::midi_out> midi_out_;

public:
    explicit LibreMidiOutputPort(const libremidi::output_port& port) {
        midi_out_ = std::make_unique<libremidi::midi_out>(libremidi::output_configuration{}, port.api);
        if (auto error = midi_out_->open_port(port); error.is_set())
            throw std::runtime_error("Failed to open MIDI output port");
    }

    ~LibreMidiOutputPort() override {
        if (midi_out_)
            midi_out_->close_port();
    }

    void addInputHandler(uapmd::ump_receiver_t, void*) override {}
    void removeInputHandler(uapmd::ump_receiver_t) override {}

    void send(uapmd_ump_t* messages, size_t sizeInBytes, uapmd_timestamp_t) override {
        if (midi_out_ && messages && sizeInBytes > 0)
            midi_out_->send_ump(reinterpret_cast<const uint32_t*>(messages), sizeInBytes / sizeof(uint32_t));
    }
};

} // namespace

std::optional<libremidi::API> uapmd::resolveLibreMidiUmpApi(const std::string& apiName) {
    auto apis = libremidi::available_ump_apis();
    if (apis.empty())
        return std::nullopt;

#ifdef _WIN32
    // Filter out Windows MIDI Services if the OS version doesn't support it
    // libremidi reports it as available if compiled with LIBREMIDI_WINMIDI,
    // but Windows MIDI Services is only supported on Windows 11 25H2 (build 26100+)
    // or Windows 11 Insider (build 27788+)
    if (!isWindowsMidiServicesSupported()) {
        auto it = std::find(apis.begin(), apis.end(), libremidi::API::WINDOWS_MIDI_SERVICES);
        if (it != apis.end())
            apis.erase(it);
    }
#endif

    if (apis.empty())
        return std::nullopt;

    if (matches_api_name(apiName, "PIPEWIRE"))
        return select_if_available(apis, libremidi::API::PIPEWIRE_UMP);
    if (matches_api_name(apiName, "ALSA"))
        return select_if_available(apis, libremidi::API::ALSA_SEQ_UMP);
    if (matches_api_name(apiName, "WINDOWS") || matches_api_name(apiName, "WINMIDI"))
        return select_if_available(apis, libremidi::API::WINDOWS_MIDI_SERVICES);

    // Treat empty, "default", and unknown names the same: use preferred ordering.
    if (apiName.empty() || matches_api_name(apiName, "default"))
        return pick_default_api(apis);

    // Unknown names fall back to default ordering to avoid surprising failures.
    return pick_default_api(apis);
}

bool uapmd::midiApiSupportsDynamicUmpEndpoints(const std::string& apiName) {
#if ANDROID
    return false;
#else
    return resolveLibreMidiUmpApi(apiName).has_value();
#endif
}

uapmd::MidiIODevice *uapmd::MidiIODevice::instance(std::string driverName) {
    (void) driverName;
    static LibreMidiIODevice impl{"PIPEWIRE", "uapmd", "uapmd", "0.0.0"};
    return &impl;
}

std::shared_ptr<uapmd::MidiIODevice> uapmd::createLibreMidiIODevice(std::string apiName,
                                                                    std::string deviceName,
                                                                    std::string manufacturer,
                                                                    std::string version,
                                                                    uint64_t sysExDelayInMicroseconds) {
    return std::make_shared<LibreMidiIODevice>(std::move(apiName),
                                               std::move(deviceName),
                                               std::move(manufacturer),
                                               std::move(version),
                                               sysExDelayInMicroseconds);
}

std::vector<uapmd::MidiPortInfo> uapmd::getMidiInputPorts(const std::vector<std::string>& excludedPortIds) {
    auto observer = makeObserver();
    std::vector<MidiPortInfo> result;
    for (const auto& port : observer.get_input_ports())
        if (!isExcluded(port, excludedPortIds))
            result.push_back({portId(port), displayName(port)});
    return result;
}

std::vector<uapmd::MidiPortInfo> uapmd::getMidiOutputPorts(const std::vector<std::string>& excludedPortIds) {
    auto observer = makeObserver();
    std::vector<MidiPortInfo> result;
    for (const auto& port : observer.get_output_ports())
        if (!isExcluded(port, excludedPortIds))
            result.push_back({portId(port), displayName(port)});
    return result;
}

std::shared_ptr<uapmd::MidiIOFeature> uapmd::openLibreMidiInputPort(const std::string& requestedPortId) {
    auto observer = makeObserver();
    for (const auto& port : observer.get_input_ports())
        if (portId(port) == requestedPortId)
            return std::make_shared<LibreMidiInputPort>(port);
    return nullptr;
}

std::shared_ptr<uapmd::MidiIOFeature> uapmd::openLibreMidiOutputPort(const std::string& requestedPortId) {
    auto observer = makeObserver();
    for (const auto& port : observer.get_output_ports())
        if (portId(port) == requestedPortId)
            return std::make_shared<LibreMidiOutputPort>(port);
    return nullptr;
}
