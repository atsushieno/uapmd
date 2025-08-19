#pragma once

#if defined(MSC_VER) // wow, that's stupid... https://stackoverflow.com/questions/5004858/why-is-stdmin-failing-when-windows-h-is-included
#define NOMINMAX
#endif

#include "priv/common.hpp"

#include "priv/plugin-catalog.hpp"
#include "priv/plugin-scanning.hpp"

#include "priv/processing-context.hpp"
#include "priv/plugin-parameter.hpp"
#include "priv/plugin-states.hpp"
#include "priv/plugin-presets.hpp"
#include "priv/port-extensibility.hpp"
#include "priv/event-loop.hpp"
#include "priv/ump-dispatcher.hpp"

#include "priv/plugin-instance.hpp"

#include "priv/plugin-format.hpp"
#include "priv/plugin-format-vst3.hpp"
#include "priv/plugin-format-au.hpp"
#include "priv/plugin-format-lv2.hpp"
#include "priv/plugin-format-clap.hpp"
