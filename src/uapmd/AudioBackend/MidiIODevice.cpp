
#include "uapmd/uapmd.hpp"
#include "impl/LibreMidiIODevice.hpp"
#include "impl/LibreMidiSupport.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <libremidi/libremidi.hpp>
#include <remidy/priv/common.hpp>

namespace {

bool matches_api_name(const std::string& value, const char* name) {
    if (value.empty() || name == nullptr)
        return false;
    return remidy_strcasecmp(value.c_str(), name) == 0;
}

std::optional<libremidi_api> select_if_available(const std::vector<libremidi::API>& apis, libremidi_api api) {
    auto desired = static_cast<libremidi::API>(api);
    if (std::find(apis.begin(), apis.end(), desired) != apis.end())
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

std::optional<libremidi_api> pick_default_api(const std::vector<libremidi::API>& apis) {
    if (apis.empty())
        return std::nullopt;

    std::vector<libremidi_api> preferred;
    if (running_in_container()) {
        preferred.push_back(PIPEWIRE_UMP);
    }
    preferred.insert(preferred.end(), {
        ALSA_SEQ_UMP,
        WINDOWS_MIDI_SERVICES,
        JACK_UMP,
        COREMIDI_UMP,
        ALSA_RAW_UMP,
        NETWORK_UMP,
        KEYBOARD_UMP
    });

    for (auto candidate : preferred) {
        if (auto match = select_if_available(apis, candidate))
            return match;
    }

    return static_cast<libremidi_api>(apis.front());
}

} // namespace

std::optional<libremidi_api> uapmd::detail::resolveLibremidiUmpApi(const std::string& apiName) {
    auto apis = libremidi::available_ump_apis();
    if (apis.empty())
        return std::nullopt;

    if (matches_api_name(apiName, "PIPEWIRE"))
        return select_if_available(apis, PIPEWIRE_UMP);
    if (matches_api_name(apiName, "ALSA"))
        return select_if_available(apis, ALSA_SEQ_UMP);
    if (matches_api_name(apiName, "WINDOWS") || matches_api_name(apiName, "WINMIDI"))
        return select_if_available(apis, WINDOWS_MIDI_SERVICES);

    // Treat empty, "default", and unknown names the same: use preferred ordering.
    if (apiName.empty() || matches_api_name(apiName, "default"))
        return pick_default_api(apis);

    // Unknown names fall back to default ordering to avoid surprising failures.
    return pick_default_api(apis);
}

bool uapmd::midiApiSupportsUmp(const std::string& apiName) {
    return detail::resolveLibremidiUmpApi(apiName).has_value();
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
