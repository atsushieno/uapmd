#pragma once

// Web stub umbrella header for uapmd that excludes MIDI-related headers
// to avoid pulling in desktop-only dependencies when building with Emscripten.

#include "uapmd/priv/CommonTypes.hpp"
#include "uapmd/priv/devices/AudioIODevice.hpp"
#include "uapmd/priv/devices/DeviceIODispatcher.hpp"
#include "uapmd/priv/plugin-api/AudioPluginInstanceAPI.hpp"
#include "uapmd/priv/plugin-api/AudioPluginHostingAPI.hpp"
#include "uapmd/priv/node-graph/AudioPluginGraph.hpp"
#include "uapmd/priv/node-graph/AudioPluginNode.hpp"
#include "uapmd/priv/node-graph/AudioPluginTrack.hpp"
#include "uapmd/priv/audio/AudioFileReader.hpp"
#include "uapmd/priv/sequencer/SequenceProcessContext.hpp"
#include "uapmd/priv/sequencer/SequencerEngine.hpp"
#include "uapmd/priv/sequencer/AudioPluginSequencer.hpp"

// Intentionally omitting MIDI headers:
//   uapmd/priv/midi/UapmdMidiDevice.hpp
//   uapmd/priv/midi/UapmdMidiCISessions.hpp
//   uapmd/priv/midi/UapmdUmpMapper.hpp

