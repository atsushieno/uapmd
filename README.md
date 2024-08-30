# What is this?

UAPMD (Universal Audio Plugin MIDI Device) is an audio plugin host that can instantiate arbitrary set of plugins and acts as a virtual MIDI 2.0 UMP device on various platforms.

## Usage

UAPMD consists of two programs:

- uapmd-service: acts as the actual virtual MIDI devices
- uapmd-setup: acts as a configuration tool

### uapmd-service

> $ uapmd-service -ac [audio-config-file] -md [midi-device-settings-file]

- `-ac`: optional. Specifies audio configuration file that can be created by `uapmd-setup`

### uapmd-setup

## Implementation

For virtual MIDI 2.0 device part it makes use of libremidi. It should work on Linux 6.5 or later, MacOS 14.0 or later, and Windows probably 11 or later (`src/service/VirtualMidiDevices`).

For audio plugin hosting, we build our own hosting later (`src/service/AudioPluginHosting`).

For `uapmd-setup` GUI we plan to use Tauri 2.0 Web UI, and connect to the service (`src/service/Controller` and `client`).
