#include <ResEmbed/Entries.h>

extern "C"
{
extern const unsigned char AppScripts_0_data[];
extern const unsigned long AppScripts_0_size;
extern const unsigned char AppScripts_1_data[];
extern const unsigned long AppScripts_1_size;
extern const unsigned char AppScripts_2_data[];
extern const unsigned long AppScripts_2_size;
}

namespace AppScripts
{
const ResEmbed::Entries& getResourceEntries()
{
    static const ResEmbed::Entries entries = {
        {AppScripts_0_data, AppScripts_0_size, "Demo.js", "AppScripts"},
        {AppScripts_1_data, AppScripts_1_size, "PluginState.js", "AppScripts"},
        {AppScripts_2_data, AppScripts_2_size, "TestDAG.js", "AppScripts"}
    };

    return entries;
}
}
