#if WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#include "CLAPHelper.hpp"
#include "../utils.hpp"
#include "clap/entry.h"

namespace remidy_clap {
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
