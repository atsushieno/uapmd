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

    inline clap_plugin_entry_t* getFactoryFromLibrary(void* module) {
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

    struct CLAPPresetInfo {
        std::string name;
        std::string load_key;
        std::string description;
    };
    
    class PresetLoader {
        std::vector<const clap_preset_discovery_provider_t*> providers{};
        std::vector<CLAPPresetInfo> presets{};

        // indexer
        std::vector<clap_preset_discovery_filetype_t> filetypes{};
        struct PresetLocation {
            uint32_t flags;
            const std::string name;
            uint32_t kind;
            const std::string location;
        };
        std::vector<PresetLocation> locations{};

        static bool declare_filetype(const struct clap_preset_discovery_indexer *indexer,
                                     const clap_preset_discovery_filetype_t     *filetype) {
            if (!filetype)
                return false;
            remidy::Logger::global()->logInfo(std::format("CLAP preset declare_filetype: {}: {}", filetype->file_extension, filetype->name).c_str());
            ((PresetLoader*) indexer->indexer_data)->filetypes.push_back(*filetype);
            return true;
        }
        static bool declare_location(const struct clap_preset_discovery_indexer *indexer,
                                     const clap_preset_discovery_location_t     *location) {
            if (!location)
                return false;
            remidy::Logger::global()->logInfo(std::format("CLAP preset declare_location: (kind: {}) (flags: {}) {}", location->kind, location->flags, location->name).c_str());
            ((PresetLoader*) indexer->indexer_data)->locations.push_back(PresetLocation{
                .flags = location->flags,
                .name = location->name,
                .kind = location->kind,
                .location = location->location
            });
            return true;
        }

        static bool declare_soundpack(const struct clap_preset_discovery_indexer *indexer,
                                      const clap_preset_discovery_soundpack_t    *soundpack) {
            std::cerr << "declare_soundpack: " << soundpack->name << std::endl;
            return false;
        }

        static const void* get_extension(const struct clap_preset_discovery_indexer *indexer,
                                         const char                                 *extension_id) {
            std::cerr << "get_extension: " << extension_id << std::endl;
            return nullptr;
        }

        // receiver

        static void on_error(const struct clap_preset_discovery_metadata_receiver *receiver,
                             int32_t                                               os_error,
                             const char                                           *error_message) {
            std::cerr << "on_error: " << os_error << " " << error_message << std::endl;
        }

        static bool begin_preset(const struct clap_preset_discovery_metadata_receiver *receiver,
                                 const char                                           *name,
                                 const char                                           *load_key) {
            return ((PresetLoader*) receiver->receiver_data)->begin_preset(name, load_key);
        }
        bool begin_preset(const char *name, const char *load_key) {
            presets.emplace_back(CLAPPresetInfo{
                    .name = name,
                    .load_key = load_key,
            });
            return true;
        }

        static void add_plugin_id(const struct clap_preset_discovery_metadata_receiver *receiver,
                                  const clap_universal_plugin_id_t                     *plugin_id) {
            // no room to use it so far.
            std::cerr << "add_plugin_id: " << plugin_id->abi << " " << plugin_id->id << std::endl;
        }

        static void set_soundpack_id(const struct clap_preset_discovery_metadata_receiver *receiver,
                                     const char *soundpack_id) {
            std::cerr << "set_soundpack_id: " << soundpack_id << std::endl;
        }

        static void set_flags(const struct clap_preset_discovery_metadata_receiver *receiver,
                              uint32_t                                              flags) {
            std::cerr << "set_flags: " << flags << std::endl;
        }

        static void add_creator(const struct clap_preset_discovery_metadata_receiver *receiver,
                                const char                                           *creator) {
            std::cerr << "add_creator: " << creator << std::endl;
        }

        static void set_description(const struct clap_preset_discovery_metadata_receiver *receiver,
                                    const char *description) {
            ((PresetLoader*) receiver->receiver_data)->presets.rbegin()->description = description;
        }

        static void set_timestamps(const struct clap_preset_discovery_metadata_receiver *receiver,
                                   clap_timestamp creation_time,
                                   clap_timestamp modification_time) {
            std::cerr << "set_timestamps: " << creation_time << " " << modification_time << std::endl;
        }

        static void add_feature(const struct clap_preset_discovery_metadata_receiver *receiver,
                                const char                                           *feature) {
            std::cerr << "add_feature: " << feature << std::endl;
        }

        static void add_extra_info(const struct clap_preset_discovery_metadata_receiver *receiver,
                                   const char                                           *key,
                                   const char                                           *value) {
            std::cerr << "add_extra_info: " << key << " = " << value << std::endl;
        }
        
    public:
        PresetLoader(clap_preset_discovery_factory* factory) {
            if (!factory)
                return;
            clap_preset_discovery_indexer_t indexer {
                    .clap_version = CLAP_VERSION,
                    .name = "Remidy",
                    .vendor = "UAPMD Project",
                    .url = "https://github.com/atsushieno/uapmd",
                    .version = "0.0.1",
                    .indexer_data = this,
                    .declare_filetype = declare_filetype,
                    .declare_location = declare_location,
                    .declare_soundpack = declare_soundpack,
                    .get_extension = get_extension
            };
            clap_preset_discovery_metadata_receiver_t receiver {
                    .receiver_data = this,
                    .on_error = on_error,
                    .begin_preset = begin_preset,
                    .add_plugin_id = add_plugin_id,
                    .set_soundpack_id = set_soundpack_id,
                    .set_flags = set_flags,
                    .add_creator = add_creator,
                    .set_description = set_description,
                    .set_timestamps = set_timestamps,
                    .add_feature = add_feature,
                    .add_extra_info = add_extra_info,
            };

            for (int32_t i = 0, n = (int32_t) factory->count(factory); i < n; i++) {
                auto desc = factory->get_descriptor(factory, i);
                if (!clap_version_is_compatible(desc->clap_version))
                    continue;
                auto provider = factory->create(factory, &indexer, desc->id);
                providers.emplace_back(provider);
                provider->init(provider);

                for (auto& location : locations)
                    if (!provider->get_metadata(provider, location.kind, location.location.c_str(), &receiver))
                        remidy::Logger::global()->logWarning(std::format("Failed to get preset metadata from provider: {} location: ({}) {}", desc->name, location.kind, location.location).c_str());
            }
            
        }
        
        ~PresetLoader() {
            for (auto provider : providers)
                provider->destroy(provider);
            providers.clear();
        }

        static std::vector<CLAPPresetInfo> loadPresets(clap_preset_discovery_factory* factory) {
            PresetLoader loader{factory};
            return loader.presets;
        }
    };
}
