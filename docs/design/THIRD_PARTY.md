# Choice of third party libraries

## Languages

- service: C++ because we chose libremidi.
  
- setup client: Rust because we choose tauri 2.0.

## MIDI 2.0 I/O

We use libremidi because none of others offers the same feature set.

- It eases development in decent cross platform manner.
- It supports MIDI 2.0 consistently.

Rust: There are too many delusive MIDI libraries that only takes name and implements nothing.

## Audio I/O

- probably we use miniaudio. It is cross-platform ready, has its own audio graph.
- choc might be an option too, but I'm unsure if its complicated buffer converters achieve good performance.

## Audio plugin hosting

- We hope to have our own multi-format hosting abstraction layer, as no one built such one in a liberal license.
  - it is so far located at `external/remidy`.
  - We use travesty for VST3.
  - We can use lilv for LV2.
  - We hope to use clap-wrapper for CLAP to load them as VST3.
    - There is no hosting library (there won't be, as it will compete against Bitwig Studio)
    - It's okay to limit CLAP ability down to what VST3 offers, it's their problem.
- Other options:
  - juce_audio_plugin_client
    - the simplest solution for single track
    - the problem is that it is basically a commercial software (AGPL for library).
    - we will need multi-track hosting and it needs extra work.
  - tracktion_engine
    - multi-track ready
    - the licensing situation is not any better than JUCE (we need multiple layers of GPL/AGPL)
    - not a safe option as the developers and the community are quite hostile against MIDI 2.0.
  - Carla
    - simple solution for single track
    - the licenses are unclear, and the core part falls into the same problem as JUCE (GPL2? 3?)

## Setup Client

- We could use saucer
  - webview.h is good enough too, though we do not have to go with C API. We might also want to use native stream requests.
  - we could use choc::WebView, but embedding non-Evergreen Microsoft WebView2 is a security risk.
