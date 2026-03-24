#include <ResEmbed/Entries.h>

extern "C"
{
extern const unsigned char AppJsLib_0_data[];
extern const unsigned long AppJsLib_0_size;
extern const unsigned char AppJsLib_1_data[];
extern const unsigned long AppJsLib_1_size;
}

namespace AppJsLib
{
const ResEmbed::Entries& getResourceEntries()
{
    static const ResEmbed::Entries entries = {
        {AppJsLib_0_data, AppJsLib_0_size, "remidy-bridge.js", "AppJsLib"},
        {AppJsLib_1_data, AppJsLib_1_size, "uapmd-api.js", "AppJsLib"}
    };

    return entries;
}
}
