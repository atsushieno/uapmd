#pragma once

#include <ResEmbed/ResEmbed.h>
#include <ResEmbed/Entries.h>

namespace AppJsLib
{
const ResEmbed::Entries& getResourceEntries();
static const ResEmbed::Initializer resourceInitializer {getResourceEntries()};
}
