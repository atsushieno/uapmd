#pragma once

#if WIN32
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif
#include "../utils.hpp"
#include "clap/entry.h"

namespace remidy_clap {
    typedef clap_plugin_entry_t (*get_clap_entry_func)();

    clap_plugin_entry_t* getFactoryFromLibrary(void* module) {
#if _WIN32
        auto sym = (clap_plugin_entry_t*) GetProcAddress((HMODULE) module, "clap_entry");
#elif __APPLE__
        auto bundle = (CFBundleRef) module;
        auto sym = (clap_plugin_entry_t*) CFBundleGetDataPointerForName(bundle, createCFString("clap_entry"));
#else
        auto sym = (clap_plugin_entry_t*) dlsym(module, "clap_entry");
#endif
        return sym;
    }

}
