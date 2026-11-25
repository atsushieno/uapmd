#pragma once

#include <ResEmbed/ResEmbed.h>
#include <ResEmbed/Entries.h>

namespace AppFonts
{
const ResEmbed::Entries& getResourceEntries();
static const ResEmbed::Initializer resourceInitializer {getResourceEntries()};
}
