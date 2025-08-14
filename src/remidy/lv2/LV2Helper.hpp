#pragma once

#include <cmath>
#include <cassert>
#include <iostream>

#include <vector>
#include <map>

#include "symap.h"
#include "zix/sem.h"
#include "zix/ring.h"
#include "zix/thread.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/port-groups/port-groups.h>
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
                    urid_patch_value{0},
                    urid_core_free_wheeling{0},
                    urid_core_integer{0};
        };

// FIXME: remove this once https://github.com/lv2/lv2/pull/74 is merged and the new version is released.
#define LV2_CORE__isSideChain LV2_CORE_PREFIX "isSideChain"

        explicit LV2ImplWorldContext(remidy::Logger* logger, LilvWorld *world) :
            logger(logger), world(world) {
            audio_port_uri_node = lilv_new_uri(world, LV2_CORE__AudioPort);
            control_port_uri_node = lilv_new_uri(world, LV2_CORE__ControlPort);
            input_port_uri_node = lilv_new_uri(world, LV2_CORE__InputPort);
            output_port_uri_node = lilv_new_uri(world, LV2_CORE__OutputPort);
            default_uri_node = lilv_new_uri(world, LV2_CORE__default);
            minimum_uri_node = lilv_new_uri(world, LV2_CORE__minimum);
            maximum_uri_node = lilv_new_uri(world, LV2_CORE__maximum);
            atom_port_uri_node = lilv_new_uri(world, LV2_ATOM__AtomPort);
            midi_event_uri_node = lilv_new_uri(world, LV2_MIDI__MidiEvent);
            patch_message_uri_node = lilv_new_uri(world, LV2_PATCH__Message);
            patch_readable_uri_node = lilv_new_uri(world, LV2_PATCH__readable);
            patch_writable_uri_node = lilv_new_uri(world, LV2_PATCH__writable);
            work_interface_uri_node = lilv_new_uri(world, LV2_WORKER__interface);
            resize_port_minimum_size_node = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
            presets_preset_node = lilv_new_uri(world, LV2_PRESETS__Preset);
            toggled_uri_node = lilv_new_uri (world, LV2_CORE__toggled);
            integer_uri_node = lilv_new_uri (world, LV2_CORE__integer);
            discrete_cv_uri_node = lilv_new_uri(world, LV2_PORT_PROPS__discreteCV);
            enumeration_uri_node = lilv_new_uri(world, LV2_CORE__enumeration);
            is_side_chain_uri_node = lilv_new_uri(world, LV2_CORE__isSideChain);
            port_group_uri_node = lilv_new_uri(world, LV2_PORT_GROUPS__group);
            scale_point_uri_node = lilv_new_uri(world, LV2_CORE__scalePoint);
            rdf_value_node = lilv_new_uri(world, LILV_NS_RDF "value");
            rdfs_label_node = lilv_new_uri(world, LILV_NS_RDFS "label");
            rdfs_range_node = lilv_new_uri(world, LILV_NS_RDFS "range");

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
            urids.urid_patch_value = map->map(map->handle, LV2_PATCH__value);
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
            lilv_node_free(default_uri_node);
            lilv_node_free(minimum_uri_node);
            lilv_node_free(maximum_uri_node);
            lilv_node_free(is_side_chain_uri_node);
            lilv_node_free(port_group_uri_node);
            lilv_node_free(midi_event_uri_node);
            lilv_node_free(patch_message_uri_node);
            lilv_node_free(patch_readable_uri_node);
            lilv_node_free(patch_writable_uri_node);
            lilv_node_free(work_interface_uri_node);
            lilv_node_free(resize_port_minimum_size_node);
            lilv_node_free(presets_preset_node);
            lilv_node_free(rdf_value_node);
            lilv_node_free(rdfs_label_node);
            lilv_node_free(rdfs_range_node);
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
                *default_uri_node, *minimum_uri_node, *maximum_uri_node,
                *toggled_uri_node, *integer_uri_node,
                *discrete_cv_uri_node,
                *enumeration_uri_node,
                *is_side_chain_uri_node,
                *port_group_uri_node,
                *scale_point_uri_node,
                *midi_event_uri_node,
                *patch_message_uri_node, *patch_readable_uri_node, *patch_writable_uri_node,
                *resize_port_minimum_size_node, *presets_preset_node,
                *work_interface_uri_node, *rdf_value_node, *rdfs_label_node, *rdfs_range_node;
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

class RemidyLV2PortMappings {
public:
    int32_t remidy_midi_in_port{-1};
    int32_t remidy_midi_out_port{-1};
    std::map<int32_t, int32_t> remidy_to_lv2_portmap{};
    std::map<int32_t, int32_t> lv2_to_remidy_portmap{};
    std::map<uint32_t, int32_t> lv2_index_to_port{};
    std::map<int32_t, int32_t> ump_group_to_atom_in_port{};
    std::map<int32_t, int32_t> atom_out_port_to_ump_group{};
    int32_t lv2_patch_in_port{-1};
    int32_t lv2_patch_out_port{-1};
};

class LV2ImplPluginContext {
public:
    LV2ImplPluginContext(LV2ImplWorldContext *statics,
                        LilvWorld *world,
                        const LilvPlugin *plugin)
            : statics(statics), world(world), plugin(plugin) {
        // They don't have default assignment...
        worker.threaded = false;
        state_worker.threaded = false;

        buildParameterList();
    }

    ~LV2ImplPluginContext() {
        // FIXME: presets?
        /*
        for (auto &p: presets)
            if (p->data)
                free(p->data);*/
        for (auto p: midi_atom_inputs)
            free(p.second);
        for (auto p: explicitly_allocated_port_buffers)
            free(p.second);
        if (control_buffer_pointers)
            free(control_buffer_pointers);
        free(dummy_raw_buffer);
    }

    LV2ImplWorldContext *statics;
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *instance{nullptr};

    RemidyLV2PortMappings mappings;

    void *dummy_raw_buffer{nullptr};

    // a ControlPort points to single float value, which can be stored in an array.
    float *control_buffer_pointers{nullptr};
    // We store ResizePort::minimumSize here, to specify sufficient Atom buffer size
    // (not to allocate local memory; it is passed as a shared memory by the local service host).
    std::map<int32_t, size_t> explicit_port_buffer_sizes{};
    // FIXME: make it a simple array so that we don't have to iterate over in every `process()`.
    std::map<int32_t, void *> explicitly_allocated_port_buffers{};
    int32_t atom_buffer_size = 0x1000;
    // They receive the Atom events that were translated from MIDI2 inputs.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_inputs{};
    // Their outputs have to be translated to MIDI2 outputs.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_outputs{};

    std::map<int32_t, LV2_Atom_Forge> midi_forges_in{};
    std::map<int32_t, LV2_Atom_Forge> midi_forges_out{};
    LV2_Atom_Forge patch_forge_in{};
    LV2_Atom_Forge patch_forge_out{};

    int32_t selected_preset_index{-1};

    std::vector<remidy::PluginParameter*> remidyParams{};
    std::map<const std::string, int32_t> remidyParamIdToEnumIndex{};
    std::vector<remidy::ParameterEnumeration*> remidyEnums{};

    void registerPortAsParameter(const LilvPlugin* plugin, const LilvPort* port) {
        std::string emptyString{};
        auto index = lilv_port_get_index(plugin, port);
        std::string idString = std::to_string(index);
        const char* portName = lilv_node_as_string(lilv_port_get_name(plugin, port));
        std::string name{portName};
        double defaultValue{0}, minValue{0}, maxValue{1};

        LilvNode *defNode{nullptr}, *minNode{nullptr}, *maxNode{nullptr}, *propertyTypeNode{nullptr}, *enumerationNode{nullptr};
        lilv_port_get_range(plugin, port, &defNode, &minNode, &maxNode);
        bool isDiscreteEnum = lilv_port_is_a(plugin, port, statics->enumeration_uri_node);
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
            defaultValue = defNode == nullptr ? 0 : lilv_node_as_float(defNode) > 0.0 ? 1 : 0;
            minValue = 0;
            maxValue = 1;
        } else if (isInteger) {
            defaultValue = defNode == nullptr ? 0 : lilv_node_as_int(defNode);
            minValue = minNode == nullptr ? 0 : lilv_node_as_int(minNode);
            maxValue = maxNode == nullptr ? 1 : lilv_node_as_int(maxNode);
        } else {
            defaultValue = defNode == nullptr ? 0 : lilv_node_as_float(defNode);
            minValue = minNode == nullptr ? 0 : lilv_node_as_float(minNode);
            maxValue = maxNode == nullptr ? 1 : lilv_node_as_float(maxNode);
        }
        remidy::PluginParameter info{index, idString, name, emptyString, defaultValue, minValue, maxValue, true, false, isDiscreteEnum};

        LilvScalePoints* scalePoints = lilv_port_get_scale_points(plugin, port);
        if (scalePoints != nullptr) {
            remidyParamIdToEnumIndex[info.stableId()] = remidyEnums.size();

            LILV_FOREACH(scale_points, spi, scalePoints) {
                auto sp = lilv_scale_points_get(scalePoints, spi);
                auto labelNode = lilv_scale_point_get_label(sp);
                auto valueNode = lilv_scale_point_get_value(sp);
                std::string label{lilv_node_as_string(labelNode)};
                auto value = lilv_node_as_float(valueNode);

                remidy::ParameterEnumeration e{label, value};
                remidyEnums.emplace_back(new remidy::ParameterEnumeration(e));
            }
            lilv_scale_points_free(scalePoints);
        } else if (isToggled) {
            remidyParamIdToEnumIndex[info.stableId()] = remidyEnums.size();
            static std::string trueValue{"true"};
            remidy::ParameterEnumeration t{trueValue, 1};
            remidyEnums.emplace_back(new remidy::ParameterEnumeration(t));

            static std::string falseValue{"false"};
            remidy::ParameterEnumeration f{falseValue, 0};
            remidyEnums.emplace_back(new remidy::ParameterEnumeration(f));
        }
        remidyParams.emplace_back(new remidy::PluginParameter(info));

        if(defNode) lilv_node_free(defNode);
        if(minNode) lilv_node_free(minNode);
        if(maxNode) lilv_node_free(maxNode);
        if(propertyTypeNode) lilv_node_free(propertyTypeNode);
    }

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

    void buildParameterList() {
        remidyParams.clear();
        remidyParamIdToEnumIndex.clear();
        remidyEnums.clear();

        for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
            auto port = lilv_plugin_get_port_by_index(plugin, p);
            if (!IS_CONTROL_PORT(plugin, port))
                continue;
            registerPortAsParameter(plugin, port);
        }
    }

    int32_t getRemidyParameterCount() { return remidyParams.size(); }
    remidy::PluginParameter getRemidyParameterInfo(int index) { return *remidyParams[index]; }
    int32_t getRemidyEnumerationCount(int32_t parameterId) {
        auto port = lilv_plugin_get_port_by_index(plugin, parameterId);
        LilvScalePoints* scalePoints = lilv_port_get_scale_points(plugin, port);
        return scalePoints != nullptr ? lilv_scale_points_size(scalePoints) : 0;
    }
    remidy::ParameterEnumeration getRemidyEnumeration(int32_t parameterId, int32_t enumIndex) {
        // FIXME: should we build something to get around string objects here?
        std::string idString = std::to_string(parameterId);
        int32_t baseIndex = remidyParamIdToEnumIndex[idString];
        return *remidyEnums[baseIndex + enumIndex];
    }

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
