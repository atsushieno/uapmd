#include <ResEmbed/Entries.h>

extern "C"
{
extern const unsigned char AppFonts_0_data[];
extern const unsigned long AppFonts_0_size;
extern const unsigned char AppFonts_1_data[];
extern const unsigned long AppFonts_1_size;
extern const unsigned char AppFonts_2_data[];
extern const unsigned long AppFonts_2_size;
}

namespace AppFonts
{
const ResEmbed::Entries& getResourceEntries()
{
    static const ResEmbed::Entries entries = {
        {AppFonts_0_data, AppFonts_0_size, "Roboto-SemiBold.ttf", "AppFonts"},
        {AppFonts_1_data, AppFonts_1_size, "fontaudio.ttf", "AppFonts"},
        {AppFonts_2_data, AppFonts_2_size, "Font Awesome 7 Free-Solid-900.otf", "AppFonts"}
    };

    return entries;
}
}
