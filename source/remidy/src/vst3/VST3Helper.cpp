#include "VST3Helper.hpp"

#if WIN32
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif
#include "../utils.hpp"

namespace remidy_vst3 {
    typedef IPluginFactory* (*get_plugin_factory_func)();

    IPluginFactory* getFactoryFromLibrary(void* module) {
#if _WIN32
        auto sym = (get_plugin_factory_func) GetProcAddress((HMODULE) module, "GetPluginFactory");
#elif __APPLE__
        auto bundle = (CFBundleRef) module;
        auto sym = (get_plugin_factory_func) CFBundleGetFunctionPointerForName(bundle, createCFString("GetPluginFactory"));
#else
        auto sym = (get_plugin_factory_func) dlsym(module, "GetPluginFactory");
#endif
        return sym();
    }

}