#pragma once

#include <cstddef>

namespace uapmd_jsfx {
    // Supplies the TrueType data the JSFX editor draws text with. See the declaration in
    // the module's public header for what it is for; this is the internal view of it.
    void setJsfxFontData(const void* data, size_t size);
    bool hasJsfxFontData();
}
