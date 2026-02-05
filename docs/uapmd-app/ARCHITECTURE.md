
# uapmd-app Overall Architecture

## Split Responsibility

Compared to remidy and uapmd, uapmd-engine and uapmd-app is organized more like a DAW.
Since we do not plan to build a comprehensive DAW but the module itself should be usable, we split various pieces of components that makes up DAW alike. Then we can focus on our own components.

If we do not effectively split this work, we will have to end up implementing everything.

## Modules

We split DAW features like this:

- remidy: audio plugin hosting
  - plugin factory interface: scanning and instantiating
  - plugin instance hosting interface: audio processing, audio buses, parameters, states, presets, UI
  - implementation:
    - VST3, AU, LV2, CLAP
- remidy-tooling: plugin scanner tooling
- remidy-gui: commonized GUI library
  - container window across platforms (used by GUI apps to host plugin UIs)
- uapmd: platform-boundable UMP device management
  - platform MIDI integration API (without implementation)
  - function block management
  - audio plugin host and instance API integration for virtual MIDI 2.0 devices (without implementation)
  - audio plugin graph API, and simple linear graph implementation (without decent DAG)
- uapmd-engine:
  - sequencer engine
  - audio file I/O API
  - audio device I/O integration API
  - project support
  - implementations
    - miniaudio audio device I/O integration
    - remidy audio plugin API integration
    - libremidi platform MIDI integration
- remidy-imgui-shared: some "shared" code that used be shared between remidy-plugin-host and uapmd-service (now unified in uapmd-app)
  - ImGui event loop
  - windowing
  - ImGui themes
- uapmd-app: GUI frontend
  - audio device settings
  - plugin list
  - MIDI keyboard
  - instance details
  - non-UI features
    - scripting runtime
  - player
