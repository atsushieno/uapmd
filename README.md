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

> $ uapmd-setup

Launches the GUI by default.
