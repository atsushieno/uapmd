#pragma once

#include "priv/CommonTypes.hpp"
// FIXME: they should be renamed from `audiograph` to `devices`
#include "priv/audiograph/AudioIODevice.hpp"
#include "priv/audiograph/MidiIODevice.hpp"
#include "priv/audiograph/DeviceIODispatcher.hpp"
// FIXME: they should be renamed from `plugingraph` to `audiograph`
#include "priv/plugingraph/AudioPluginGraph.hpp"
#include "priv/plugingraph/AudioPluginHostPAL.hpp"
#include "priv/plugingraph/AudioPluginNode.hpp"
#include "priv/plugingraph/AudioPluginTrack.hpp"
#include "priv/sequencer/SequenceProcessContext.hpp"
#include "priv/sequencer/SequenceProcessor.hpp"
#include "priv/sequencer/AudioPluginSequencer.hpp"
#include "priv/midi/PlatformVirtualMidiDevice.hpp"
#include "priv/midi/UapmdMidiDevice.hpp"
#include "priv/midi/UapmdMidiCISessions.hpp"
#include "priv/midi/UapmdUmpMapper.hpp"
