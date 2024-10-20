#pragma once

#ifndef AAP_LV2_INTERNAL_INCLUDED
#define AAP_LV2_INTERNAL_INCLUDED 1

#include <unistd.h>
#include <dlfcn.h>
#include <cmath>
#include <ctime>
#include <cstring>
#include <cassert>
#include <iostream>

#include <memory>
#include <vector>
#include <map>
#include <string>

#include "symap.h"
#include "zix/sem.h"
#include "zix/ring.h"
#include "zix/thread.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/worker/worker.h>
#include <lv2/log/log.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/port-props/port-props.h>
#include <lv2/state/state.h>
#include <lv2/presets/presets.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/patch/patch.h>
#include <format>

#include "remidy.hpp"

namespace remidy_lv2 {

    inline int log_vprintf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, va_list ap) {
        auto logger = (remidy::Logger*) handle;
        logger->logv(remidy::Logger::LogLevel::INFO, fmt, ap);
        return 0;
    }

    inline int log_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...) {
        va_list ap;
        va_start (ap, fmt);
        int ret = log_vprintf(handle, type, fmt, ap);
        va_end (ap);
        return ret;
    }


    class LV2ImplFeatures {
    public:
        LV2_URID_Map urid_map_feature_data{};
        LV2_URID_Unmap urid_unmap_feature_data{};
        LV2_Worker_Schedule worker_schedule_data{};
        LV2_Worker_Schedule state_worker_schedule_data{};
        LV2_Log_Log logData{nullptr, log_printf, log_vprintf};

        const int minBlockLengthValue = 128;
        // FIXME: this should not be a magic number, but lowering the value causes aap-sfizz crash.
        //  Needs some investigation.
        const int maxBlockLengthValue = 8192;

        LV2_Options_Option options[3] {
            {},
            {},
            LV2_Options_Option{LV2_OPTIONS_BLANK, 0, 0, 0, 0}
        };
        LV2_Options_Option* minBlockLengthOption{&options[0]};
        LV2_Options_Option* maxBlockLengthOption{&options[1]};

        LV2_Feature mapFeature{LV2_URID__map, &urid_map_feature_data};
        LV2_Feature unmapFeature{LV2_URID__unmap, &urid_unmap_feature_data};
        LV2_Feature logFeature{LV2_LOG__log, &logData};
        LV2_Feature bufSizeFeature{LV2_BUF_SIZE__boundedBlockLength, nullptr};
        LV2_Feature optionsFeature{LV2_OPTIONS__options, &options};
        LV2_Feature workerFeature{LV2_WORKER__schedule, &worker_schedule_data};
        LV2_Feature stateWorkerFeature{LV2_WORKER__schedule, &state_worker_schedule_data};
        LV2_Feature threadSafeRestoreFeature{LV2_STATE__threadSafeRestore, nullptr};
        // cf. https://github.com/x42/lv2vst/pull/1
        LV2_Feature boundedBlockLengthFeature{LV2_BUF_SIZE__boundedBlockLength, nullptr};

        LV2_Feature* features[10] {
            &mapFeature,
            &unmapFeature,
            &logFeature,
            &bufSizeFeature,
            &optionsFeature,
            &workerFeature,
            &stateWorkerFeature,
            &threadSafeRestoreFeature,
            &boundedBlockLengthFeature,
            nullptr
        };
    };

    static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char* uri);
    static const char* unmap_uri(LV2_URID_Unmap_Handle handle, LV2_URID urid);

    struct LV2ImplWorldContext {
        struct URIDs {
            LV2_URID urid_atom_sequence_type{0},
                    urid_midi_event_type{0},
                    urid_time_frame{0},
                    urid_atom_float_type{0},
                    urid_atom_bool_type{0},
                    urid_atom_int_type{0},
                    urid_patch_set{0},
                    urid_patch_property{0},
                    urid_core_free_wheeling{0},
                    urid_core_integer{0};
        };

        explicit LV2ImplWorldContext(remidy::Logger* logger, LilvWorld *world) :
            logger(logger), world(world) {
            audio_port_uri_node = lilv_new_uri(world, LV2_CORE__AudioPort);
            control_port_uri_node = lilv_new_uri(world, LV2_CORE__ControlPort);
            input_port_uri_node = lilv_new_uri(world, LV2_CORE__InputPort);
            output_port_uri_node = lilv_new_uri(world, LV2_CORE__OutputPort);
            default_uri_node = lilv_new_uri(world, LV2_CORE__default);
            atom_port_uri_node = lilv_new_uri(world, LV2_ATOM__AtomPort);
            midi_event_uri_node = lilv_new_uri(world, LV2_MIDI__MidiEvent);
            patch_message_uri_node = lilv_new_uri(world, LV2_PATCH__Message);
            work_interface_uri_node = lilv_new_uri(world, LV2_WORKER__interface);
            resize_port_minimum_size_node = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
            presets_preset_node = lilv_new_uri(world, LV2_PRESETS__Preset);
            toggled_uri_node = lilv_new_uri (world, LV2_CORE__toggled);
            integer_uri_node = lilv_new_uri (world, LV2_CORE__integer);
            discrete_cv_uri_node = lilv_new_uri(world, LV2_PORT_PROPS__discreteCV);
            rdfs_label_node = lilv_new_uri(world, LILV_NS_RDFS "label");

            features.urid_map_feature_data.handle = this;
            features.urid_map_feature_data.map = map_uri;
            features.urid_unmap_feature_data.handle = this;
            features.urid_unmap_feature_data.unmap = unmap_uri;

            symap = symap_new();
            if (zix_sem_init(&symap_lock, 1))
                throw std::runtime_error("Failed to initialize semaphore (symap).");

            auto map = &features.urid_map_feature_data;
            urids.urid_atom_sequence_type = map->map(map->handle, LV2_ATOM__Sequence);
            urids.urid_midi_event_type = map->map(map->handle, LV2_MIDI__MidiEvent);
            urids.urid_time_frame = map->map(map->handle, LV2_ATOM__frameTime);
            urids.urid_atom_float_type = map->map(map->handle, LV2_ATOM__Float);
            urids.urid_atom_bool_type = map->map(map->handle, LV2_ATOM__Bool);
            urids.urid_atom_int_type = map->map(map->handle, LV2_ATOM__Int);
            urids.urid_patch_set = map->map(map->handle, LV2_PATCH__Set);
            urids.urid_patch_property = map->map(map->handle, LV2_PATCH__property);
            urids.urid_core_free_wheeling = map->map(map->handle, LV2_CORE__freeWheeling);

            features.logFeature.data = logger;

            *features.minBlockLengthOption = {LV2_OPTIONS_INSTANCE,
                                                  0,
                                                  map_uri(this, LV2_BUF_SIZE__minBlockLength),
                                                  sizeof(int),
                                                  map_uri(this, LV2_ATOM__Int),
                                                  &features.minBlockLengthValue};
            *features.maxBlockLengthOption = {LV2_OPTIONS_INSTANCE,
                                                  0,
                                                  map_uri(this, LV2_BUF_SIZE__maxBlockLength),
                                                  sizeof(int),
                                                  map_uri(this, LV2_ATOM__Int),
                                                  &features.maxBlockLengthValue};
        }

        ~LV2ImplWorldContext() {
            symap_free(symap);

            lilv_node_free(audio_port_uri_node);
            lilv_node_free(control_port_uri_node);
            lilv_node_free(atom_port_uri_node);
            lilv_node_free(input_port_uri_node);
            lilv_node_free(output_port_uri_node);
            lilv_node_free(midi_event_uri_node);
            lilv_node_free(patch_message_uri_node);
            lilv_node_free(work_interface_uri_node);
            lilv_node_free(resize_port_minimum_size_node);
            lilv_node_free(presets_preset_node);
            lilv_node_free(rdfs_label_node);
        }

        // formerly stateFeaturesList()
        std::vector<LV2_Feature*> getFeaturesForState() {
            LV2_Feature *list[]{
                &features.mapFeature,
                &features.unmapFeature,
                &features.logFeature,
                &features.optionsFeature,
                &features.threadSafeRestoreFeature,
                &features.stateWorkerFeature,
                nullptr
        };
            std::vector<LV2_Feature*> ret{};
            for (auto e : list)
                ret.emplace_back(e);
            return ret;
        }

        remidy::Logger* logger;
        LilvWorld* world;
        LV2ImplFeatures features;
        LilvNode *audio_port_uri_node, *control_port_uri_node, *atom_port_uri_node,
                *input_port_uri_node, *output_port_uri_node,
                *default_uri_node,
                *toggled_uri_node, *integer_uri_node,
                *discrete_cv_uri_node,
                *midi_event_uri_node, *patch_message_uri_node,
                *resize_port_minimum_size_node, *presets_preset_node,
                *work_interface_uri_node, *rdfs_label_node;
        URIDs urids;

        Symap *symap{nullptr};          ///< URI map
        ZixSem symap_lock;     ///< Lock for URI map
    };

    static LV2_URID
    map_uri(LV2_URID_Map_Handle handle,
            const char *uri) {
        auto ctx = (LV2ImplWorldContext *) handle;
        zix_sem_wait(&ctx->symap_lock);
        const LV2_URID id = symap_map(ctx->symap, uri);
        zix_sem_post(&ctx->symap_lock);
        return id;
    }

    static const char *
    unmap_uri(LV2_URID_Unmap_Handle handle,
              LV2_URID urid) {
        auto ctx = (LV2ImplWorldContext *) handle;
        zix_sem_wait(&ctx->symap_lock);
        const char *uri = symap_unmap(ctx->symap, urid);
        zix_sem_post(&ctx->symap_lock);
        return uri;
    }


class LV2ImplPluginContext;

// imported from jalv
struct JalvWorker {
    LV2ImplPluginContext *ctx{};       ///< Pointer back to <del>Jalv</del>LV2ImplWorldContext
    ZixRing *requests{nullptr};   ///< Requests to the worker
    ZixRing *responses{nullptr};  ///< Responses from the worker
    void *response{};   ///< Worker response buffer
    ZixSem sem{};        ///< Worker semaphore
    ZixThread thread{};     ///< Worker thread
    const LV2_Worker_Interface *iface{nullptr};      ///< Plugin worker interface
    bool threaded{};   ///< Run work in another thread
};

void
jalv_worker_emit_responses(JalvWorker *worker, LilvInstance *instance);

// FIXME: these types named AAP-someghing are what I just copy-pasted from aap-lv2 and not quite ready to use.
/*
class AAPLV2PortMappings {
public:
    int32_t aap_midi_in_port{-1};
    int32_t aap_midi_out_port{-1};
    std::map<int32_t, int32_t> aap_to_lv2_portmap{};
    std::map<int32_t, int32_t> lv2_to_aap_portmap{};
    std::map<uint32_t, int32_t> lv2_index_to_port{};
    std::map<int32_t, int32_t> ump_group_to_atom_in_port{};
    std::map<int32_t, int32_t> atom_out_port_to_ump_group{};
    int32_t lv2_patch_in_port{-1};
    int32_t lv2_patch_out_port{-1};
};
*/

// removed sampleRate from aap-lv2 version...
class LV2ImplPluginContext {
public:
    LV2ImplPluginContext(LV2ImplWorldContext *statics,
                        LilvWorld *world,
                        const LilvPlugin *plugin)
            : statics(statics), world(world), plugin(plugin) {
        // They don't have default assignment...
        worker.threaded = false;
        state_worker.threaded = false;

        //buildParameterList();
    }

    ~LV2ImplPluginContext() {
        /*
        for (auto &p: presets)
            if (p->data)
                free(p->data);
        for (auto p: midi_atom_inputs)
            free(p.second);
        for (auto p: explicitly_allocated_port_buffers)
            free(p.second);
        if (control_buffer_pointers)
            free(control_buffer_pointers);
        free(dummy_raw_buffer);
        */
    }

    LV2ImplWorldContext *statics;
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *instance{nullptr};

    /*
    std::string aap_plugin_id{};
    AAPLV2PortMappings mappings;

    void *dummy_raw_buffer{nullptr};

    // a ControlPort points to single float value, which can be stored in an array.
    float *control_buffer_pointers{nullptr};
    // We store ResizePort::minimumSize here, to specify sufficient Atom buffer size
    // (not to allocate local memory; it is passed as a shared memory by the local service host).
    std::map<int32_t, size_t> explicit_port_buffer_sizes{};
    // FIXME: make it a simple array so that we don't have to iterate over in every `process()`.
    std::map<int32_t, void *> explicitly_allocated_port_buffers{};
    int32_t atom_buffer_size = 0x1000;
    // They receive the Atom events that were translated from AAP MIDI2 inputs.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_inputs{};
    // Their outputs have to be translated to AAP MIDI2 outputs.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_outputs{};

    std::map<int32_t, LV2_Atom_Forge> midi_forges_in{};
    std::map<int32_t, LV2_Atom_Forge> midi_forges_out{};
    LV2_Atom_Forge patch_forge_in{};
    LV2_Atom_Forge patch_forge_out{};

    int32_t selected_preset_index{-1};

    std::map<int32_t,int32_t> aapParamIdToEnumIndex{};

    void registerParameter(const LilvPlugin* plugin, const LilvPort* port) {
        aap_parameter_info_t info{0, {}, {}, 0, 1, 0, 0};
        info.path[0] = '\0';
        info.stable_id = static_cast<int16_t>(lilv_port_get_index(plugin, port));
        auto nameMax = sizeof(info.display_name);
        auto nameNode = lilv_port_get_name(plugin, port);
        const char* paramName = lilv_node_as_string(nameNode);
        strncpy(info.display_name, paramName, nameMax);

        LilvNode *defNode{nullptr}, *minNode{nullptr}, *maxNode{nullptr}, *propertyTypeNode{nullptr};
        lilv_port_get_range(plugin, port, &defNode, &minNode, &maxNode);
        LilvNodes *portProps = lilv_port_get_properties(plugin, port);
        bool isInteger{false};
        bool isToggled{false};
        LILV_FOREACH(nodes, pp, portProps) {
            auto portProp = lilv_nodes_get(portProps, pp);
            if (lilv_node_equals(portProp, statics->integer_uri_node))
                isInteger = true;
            if (lilv_node_equals(portProp, statics->toggled_uri_node))
                isToggled = true;
        }
        if (isToggled) {
            info.default_value = defNode == nullptr ? 0 : lilv_node_as_float(defNode) > 0.0 ? 1 : 0;
            info.min_value = 0;
            info.max_value = 1;
        } else if (isInteger) {
            info.default_value = defNode == nullptr ? 0 : lilv_node_as_int(defNode);
            info.min_value = minNode == nullptr ? 0 : lilv_node_as_int(minNode);
            info.max_value = maxNode == nullptr ? 1 : lilv_node_as_int(maxNode);
        } else {
            info.default_value = defNode == nullptr ? 0 : lilv_node_as_float(defNode);
            info.min_value = minNode == nullptr ? 0 : lilv_node_as_float(minNode);
            info.max_value = maxNode == nullptr ? 1 : lilv_node_as_float(maxNode);
        }

        LilvScalePoints* scalePoints = lilv_port_get_scale_points(plugin, port);
        if (scalePoints != nullptr) {
            aapParamIdToEnumIndex[info.stable_id] = aapEnums.size();

            LILV_FOREACH(scale_points, spi, scalePoints) {
                auto sp = lilv_scale_points_get(scalePoints, spi);
                auto labelNode = lilv_scale_point_get_label(sp);
                auto valueNode = lilv_scale_point_get_value(sp);
                auto label = lilv_node_as_string(labelNode);
                auto value = lilv_node_as_float(valueNode);

                aap_parameter_enum_t e;
                e.value = value;
                strncpy(e.name, label, sizeof(e.name));
                aapEnums.emplace_back(new aap_parameter_enum_t(e));
            }
            lilv_scale_points_free(scalePoints);
        } else if (isToggled) {
            aapParamIdToEnumIndex[info.stable_id] = aapEnums.size();
            aap_parameter_enum_t t;
            t.value = 1;
            strncpy(t.name, "true", sizeof(t.name));
            aapEnums.emplace_back(new aap_parameter_enum_t(t));

            aap_parameter_enum_t f;
            f.value = 0;
            strncpy(f.name, "false", sizeof(f.name));
            aapEnums.emplace_back(new aap_parameter_enum_t(f));
        }
        aapParams.emplace_back(new aap_parameter_info_t(info));

        if(defNode) lilv_node_free(defNode);
        if(minNode) lilv_node_free(minNode);
        if(maxNode) lilv_node_free(maxNode);
        if(propertyTypeNode) lilv_node_free(propertyTypeNode);
    }
*/

#define PORTCHECKER_SINGLE(_name_,_type_) inline bool _name_ (const LilvPlugin* plugin, const LilvPort* port) { return lilv_port_is_a (plugin, port, statics->_type_); }
#define PORTCHECKER_AND(_name_,_cond1_,_cond2_) inline bool _name_ (const LilvPlugin* plugin, const LilvPort* port) { return _cond1_ (plugin, port) && _cond2_ (plugin, port); }

        PORTCHECKER_SINGLE (IS_CONTROL_PORT, control_port_uri_node)
        PORTCHECKER_SINGLE (IS_AUDIO_PORT, audio_port_uri_node)
        PORTCHECKER_SINGLE (IS_INPUT_PORT, input_port_uri_node)
        PORTCHECKER_SINGLE (IS_OUTPUT_PORT, output_port_uri_node)
        PORTCHECKER_SINGLE (IS_ATOM_PORT, atom_port_uri_node)
        PORTCHECKER_AND (IS_AUDIO_IN, IS_AUDIO_PORT, IS_INPUT_PORT)
        PORTCHECKER_AND (IS_AUDIO_OUT, IS_AUDIO_PORT, IS_OUTPUT_PORT)
        PORTCHECKER_AND (IS_ATOM_IN, IS_ATOM_PORT, IS_INPUT_PORT)
        PORTCHECKER_AND (IS_ATOM_OUT, IS_ATOM_PORT, IS_OUTPUT_PORT)

/*
    void buildParameterList() {
        aapParams.clear();
        aapParamIdToEnumIndex.clear();
        aapEnums.clear();

        for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
            auto port = lilv_plugin_get_port_by_index(plugin, p);
            if (!IS_CONTROL_PORT(plugin, port))
                continue;
            registerParameter(plugin, port);
        }
    }

    int32_t getAAPParameterCount() { return aapParams.size(); }
    aap_parameter_info_t getAAPParameterInfo(int index) { return *aapParams[index]; }
    double getAAPParameterProperty(int32_t parameterId, int32_t propertyId) {
        for (auto info: aapParams) {
            if (info->stable_id == parameterId) {
                switch (propertyId) {
                    case AAP_PARAMETER_PROPERTY_MIN_VALUE:
                        return info->min_value;
                    case AAP_PARAMETER_PROPERTY_MAX_VALUE:
                        return info->max_value;
                    case AAP_PARAMETER_PROPERTY_DEFAULT_VALUE:
                        return info->default_value;
                    case AAP_PARAMETER_PROPERTY_IS_DISCRETE: {
                        auto port = lilv_plugin_get_port_by_index(plugin, parameterId);
                        auto value = lilv_port_get_value(plugin, port, statics->discrete_cv_uri_node);
                        if (value != nullptr)
                            return lilv_node_as_float((LilvNode*) value);
                    }
                        // LV2 does not have it (yet?)
                    case AAP_PARAMETER_PROPERTY_PRIORITY:
                        return 0;
                }
            }
        }
        return 0;
    }
    int32_t getAAPEnumerationCount(int32_t parameterId) {
        auto port = lilv_plugin_get_port_by_index(plugin, parameterId);
        LilvScalePoints* scalePoints = lilv_port_get_scale_points(plugin, port);
        return scalePoints != nullptr ? lilv_scale_points_size(scalePoints) : 0;
    }
    aap_parameter_enum_t getAAPEnumeration(int32_t parameterId, int32_t enumIndex) {
        int32_t baseIndex = aapParamIdToEnumIndex[parameterId];
        return *aapEnums[baseIndex + enumIndex];
    }
    */

    // from jalv codebase
    JalvWorker worker;         ///< Worker thread implementation
    JalvWorker state_worker;   ///< Synchronous worker for state restore
    ZixSem work_lock;      ///< Lock for plugin work() method
    bool safe_restore{false};   ///< Plugin restore() is thread-safe
    bool exit{false};
};

    LilvInstance* instantiate_plugin(
        LV2ImplWorldContext* worldContext,
        LV2ImplPluginContext* pluginContext,
        const LilvPlugin* plugin,
        int sampleRate,
        bool offlineMode);
}

#endif // ifndef AAP_LV2_INTERNAL_INCLUDED