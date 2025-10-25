#if defined(MSC_VER) // wow, that's stupid... https://stackoverflow.com/questions/5004858/why-is-stdmin-failing-when-windows-h-is-included
#define NOMINMAX
#endif

#include <lv2/core/lv2.h>


#include <memory>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>

#include <lilv/lilv.h>
#include <lv2/options/options.h>
#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#include <zix/sem.h>

#include "LV2Helper.hpp"

namespace remidy_lv2 {

// imported from jalv_internal.h
typedef int (*PresetSink)(LV2ImplPluginContext*           jalv,
                          const LilvNode* node,
                          const LilvNode* title,
                          void*           data);

// imported from jalv/src/state.c with some changes to match LV2ImplPluginContext
int
jalv_load_presets(LV2ImplPluginContext* jalv, PresetSink sink, void* data)
{
    LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
                                                 jalv->statics->presets_preset_node);
    LILV_FOREACH(nodes, i, presets) {
        const LilvNode* preset = lilv_nodes_get(presets, i);
        lilv_world_load_resource(jalv->world, preset);
        if (!sink) {
            continue;
        }

        LilvNodes* labels = lilv_world_find_nodes(
                jalv->world, preset, jalv->statics->rdfs_label_node, nullptr);
        if (labels) {
            const LilvNode* label = lilv_nodes_get_first(labels);
            sink(jalv, preset, label, data);
            lilv_nodes_free(labels);
        } else {
            fprintf(stderr, "Preset <%s> has no rdfs:label\n",
                    lilv_node_as_string(lilv_nodes_get(presets, i)));
        }
    }
    lilv_nodes_free(presets);

    return 0;
}

// The code below (jalv_worker_xxx) is copied from jalv worker.c and then made minimum required changes.

static LV2_Worker_Status
jalv_worker_respond(LV2_Worker_Respond_Handle handle,
                    uint32_t                  size,
                    const void*               data)
{
    JalvWorker* worker = (JalvWorker*)handle;
    zix_ring_write(worker->responses, (const char*)&size, sizeof(size));
    zix_ring_write(worker->responses, (const char*)data, size);
    return LV2_WORKER_SUCCESS;
}

static ZixThreadResult
ZIX_THREAD_FUNC worker_func(void* data)
{
    JalvWorker* worker = (JalvWorker*)data;
    LV2ImplPluginContext*       jalv   = worker->ctx;
    void*       buf    = NULL;
    while (true) {
        zix_sem_wait(&worker->sem);
        if (jalv->exit) {
            break;
        }

        uint32_t size = 0;
        zix_ring_read(worker->requests, (char*)&size, sizeof(size));

        if (!(buf = realloc(buf, size))) {
            fprintf(stderr, "error: realloc() failed\n");
            free(buf);
            return NULL;
        }

        zix_ring_read(worker->requests, (char*)buf, size);

        zix_sem_wait(&jalv->work_lock);
        worker->iface->work(
                jalv->instance->lv2_handle, jalv_worker_respond, worker, size, buf);
        zix_sem_post(&jalv->work_lock);
    }

    free(buf);
    return NULL;
}

void
jalv_worker_init(LV2ImplWorldContext*                       ZIX_UNUSED(jalv),
                 JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded)
{
    worker->iface = iface;
    worker->threaded = threaded;
    if (threaded) {
        zix_thread_create(&worker->thread, 4096, worker_func, worker);
        worker->requests = zix_ring_new(zix_default_allocator(), 4096);
        zix_ring_mlock(worker->requests);
    }
    worker->responses = zix_ring_new(zix_default_allocator(), 4096);
    worker->response  = malloc(4096);
    zix_ring_mlock(worker->responses);
}

void
jalv_worker_finish(JalvWorker* worker)
{
    if (worker->threaded) {
        zix_sem_post(&worker->sem);
        zix_thread_join(worker->thread);
    }
}

void
jalv_worker_destroy(JalvWorker* worker)
{
    if (worker->requests) {
        if (worker->threaded) {
            zix_ring_free(worker->requests);
        }
        zix_ring_free(worker->responses);
        free(worker->response);
    }
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data)
{
    JalvWorker* worker = (JalvWorker*)handle;
    LV2ImplPluginContext*       jalv   = worker->ctx;
    if (worker->threaded) {
        // Schedule a request to be executed by the worker thread
        zix_ring_write(worker->requests, (const char*)&size, sizeof(size));
        zix_ring_write(worker->requests, (const char*)data, size);
        zix_sem_post(&worker->sem);
    } else {
        // Execute work immediately in this thread
        zix_sem_wait(&jalv->work_lock);
        worker->iface->work(
                jalv->instance->lv2_handle, jalv_worker_respond, worker, size, data);
        zix_sem_post(&jalv->work_lock);
    }
    return LV2_WORKER_SUCCESS;
}

void
jalv_worker_emit_responses(JalvWorker* worker, LilvInstance* instance)
{
    if (worker->responses) {
        uint32_t read_space = zix_ring_read_space(worker->responses);
        while (read_space) {
            uint32_t size = 0;
            zix_ring_read(worker->responses, (char*)&size, sizeof(size));

            zix_ring_read(worker->responses, (char*)worker->response, size);

            worker->iface->work_response(
                    instance->lv2_handle, size, worker->response);

            read_space -= sizeof(size) + size;
        }
    }
}
// end of jalv worker code.

// State extension

    /*
const void* aap_lv2_get_port_value(
        const char* port_symbol, void* user_data, uint32_t* size, uint32_t* type)
{
    auto l = (AAPLV2PluginContext *) user_data;
    auto uri = lilv_new_string(l->world, port_symbol);
    auto port = lilv_plugin_get_port_by_symbol(l->plugin, uri);
    lilv_node_free(uri);
    int index = lilv_port_get_index(l->plugin, port);

    // FIXME: preserve buffer in context, and retrieve from there.
    auto data = l->cached_buffer->get_buffer(*l->cached_buffer, index);

    // FIXME: implement correctly
    *size = sizeof(float);
    *type = l->urids.urid_atom_float_type;
    return data;
}

void aap_lv2_set_port_value(
        const char* port_symbol, void* user_data, const void* value, uint32_t size, uint32_t type)
{
    auto l = (AAPLV2PluginContext *) user_data;
    assert(l->instance_state != AAP_LV2_INSTANCE_STATE_INITIAL); // must be at prepared or later.

    auto uri = lilv_new_string(l->world, port_symbol);
    auto port = lilv_plugin_get_port_by_symbol(l->plugin, uri);
    auto lv2Port = lilv_port_get_index(l->plugin, port);
    if (lv2Port >= 0) {
        auto aapPort = l->mappings.lv2_to_aap_portmap[(int32_t) lv2Port];
        if (aapPort >= 0) {
            auto data = l->cached_buffer->get_buffer(*l->cached_buffer, aapPort);
            memcpy(data, value, size);
        } else {
            // it is hopefully a float ControlPort...
            // also note: https://github.com/atsushieno/aap-lv2/issues/7
            auto data = l->control_buffer_pointers + lv2Port;
            memcpy(data, value, size);
        }
    }
    else
        aap::a_log_f(AAP_LOG_LEVEL_WARN, AAP_LV2_TAG, "State contains invalid LV2 port specifier: %s", lilv_node_as_uri(uri));
    lilv_node_free(uri);
}

size_t aap_lv2_get_state_size(aap_state_extension_t* ext, AndroidAudioPlugin* plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    auto features = l->stateFeaturesList();
    LilvState *state = lilv_state_new_from_instance(l->plugin, l->instance, &l->features.urid_map_feature_data,
                                                    nullptr, nullptr, nullptr, nullptr, aap_lv2_get_port_value, l, 0, features.get());
    auto nameNode = lilv_plugin_get_name(l->plugin);
    std::string stateUriBase{"urn:aap_state:"};
    auto nameChars = lilv_node_as_string(nameNode);
    std::string nameString{nameChars};
    std::string stateUri = stateUriBase + nameString;
    lilv_node_free(nameNode);
    auto stateString = lilv_state_to_string(l->world, &l->features.urid_map_feature_data, &l->features.urid_unmap_feature_data,
                                            state, stateUri.c_str(), nullptr);
    auto ret = strlen(stateString);
    lilv_state_delete(l->world, state);
    return ret;
}

void aap_lv2_get_state(aap_state_extension_t* ext, AndroidAudioPlugin* plugin, aap_state_t *result) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    auto features = l->stateFeaturesList();
    LilvState *state = lilv_state_new_from_instance(l->plugin, l->instance, &l->features.urid_map_feature_data,
                                                    nullptr, nullptr, nullptr, nullptr, aap_lv2_get_port_value, l, 0, features.get());
    auto nameNode = lilv_plugin_get_name(l->plugin);
    std::string stateUriBase{"urn:aap_state:"};
    auto nameChars = lilv_node_as_string(nameNode);
    std::string nameString{nameChars};
    std::string stateUri = stateUriBase + nameString;
    lilv_node_free(nameNode);
    auto stateString = lilv_state_to_string(l->world, &l->features.urid_map_feature_data, &l->features.urid_unmap_feature_data,
                                            state, stateUri.c_str(), nullptr);
    result->data = strdup(stateString);
    result->data_size = strlen(stateString);
    lilv_state_delete(l->world, state);
}

void aap_lv2_set_state(aap_state_extension_t* ext, AndroidAudioPlugin* plugin, aap_state_t *input) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    LilvState *state = lilv_state_new_from_string(l->world, &l->features.urid_map_feature_data, (const char*) input->data);
    auto features = l->stateFeaturesList();
    lilv_state_restore(state, l->instance, aap_lv2_set_port_value, l, 0, features.get());
    lilv_state_delete(l->world, state);
}

// Presets extension

int32_t aap_lv2_on_preset_loaded(Jalv* jalv, const LilvNode* node, const LilvNode* title, void* data) {
    auto name = lilv_node_as_string(title);
    auto uridMap = &jalv->features.urid_map_feature_data;
    auto uridUnmap = &jalv->features.urid_unmap_feature_data;
    auto state = lilv_state_new_from_world(jalv->world, uridMap, node);
    auto stateData = lilv_state_to_string(jalv->world, uridMap, uridUnmap, state, lilv_node_as_string(node), nullptr);

    aap_preset_t preset;
    preset.id = (int32_t) jalv->presets.size();
    strncpy(preset.name, name, AAP_PRESETS_EXTENSION_MAX_NAME_LENGTH);
    jalv->presets.emplace_back(std::make_unique<AAPPresetAndLv2Binary>(preset, strdup(stateData)));
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "AAP-LV2", "aap_lv2_on_preset_loaded. %s: %s", name, lilv_node_as_string(node));
    free((void *) name);
    free(stateData);
    lilv_state_free(state);
    return 0;
}

void aap_lv2_ensure_preset_loaded(AAPLV2PluginContext *ctx) {
    if (ctx->presets.size() == 0)
        jalv_load_presets(ctx, aap_lv2_on_preset_loaded, nullptr);
}

int32_t aap_lv2_get_preset_count(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    return ctx->presets.size();
}
void aap_lv2_get_preset(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin, int32_t index, aap_preset_t* destination, aapxs_completion_callback callback, void* callbackContext) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    auto preset = ctx->presets[index].get();
    strncpy(destination->name, preset->preset.name, AAP_PRESETS_EXTENSION_MAX_NAME_LENGTH);
    if (callback)
        callback(callbackContext, plugin);
}
int32_t aap_lv2_get_preset_index(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    return ctx->selected_preset_index;
}
void aap_lv2_set_preset_index(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    ctx->selected_preset_index = index;

    for (auto& p : ctx->presets) {
        if (p->preset.id == index) {
            auto state = lilv_state_new_from_string(ctx->world,
                                                    &ctx->features.urid_map_feature_data,
                                                    (const char *) p->data);
            lilv_state_restore(state, ctx->instance, aap_lv2_set_port_value, ctx, 0, ctx->stateFeaturesList().get());
            break;
        }
    }
}

// parameters extension

int32_t aap_lv2_get_parameter_count(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPParameterCount();
}

aap_parameter_info_t aap_lv2_get_parameter(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPParameterInfo(index);
}

double aap_lv2_get_parameter_property(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t parameterId, int32_t propertyId) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPParameterProperty(parameterId, propertyId);
}

int32_t aap_lv2_get_enumeration_count(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t parameterId) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPEnumerationCount(parameterId);
}

aap_parameter_enum_t aap_lv2_get_enumeration(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t parameterId, int32_t enumIndex) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPEnumeration(parameterId, enumIndex);
}

aap_parameters_extension_t params_ext{nullptr,
                                      aap_lv2_get_parameter_count,
                                      aap_lv2_get_parameter,
                                      aap_lv2_get_parameter_property,
                                      aap_lv2_get_enumeration_count,
                                      aap_lv2_get_enumeration
                                      };

aap_state_extension_t state_ext{nullptr,
                                aap_lv2_get_state_size,
                                aap_lv2_get_state,
                                aap_lv2_set_state};

aap_presets_extension_t presets_ext{nullptr,
                                    aap_lv2_get_preset_count,
                                    aap_lv2_get_preset,
                                    aap_lv2_get_preset_index,
                                    aap_lv2_set_preset_index};

void* aap_lv2_plugin_get_extension(AndroidAudioPlugin *plugin, const char *uri) {
    if (strcmp(uri, AAP_PARAMETERS_EXTENSION_URI) == 0) {
        return &params_ext;
    }
    if (strcmp(uri, AAP_STATE_EXTENSION_URI) == 0) {
        return &state_ext;
    }
    if (strcmp(uri, AAP_PRESETS_EXTENSION_URI) == 0) {
        return &presets_ext;
    }
    return nullptr;
}

aap_plugin_info_t aap_lv2_plugin_get_plugin_info(AndroidAudioPlugin* plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    auto hostExt = (aap_host_plugin_info_extension_t*) ctx->aap_host->get_extension(ctx->aap_host, AAP_PLUGIN_INFO_EXTENSION_URI);
    return hostExt->get(hostExt, ctx->aap_host, ctx->aap_plugin_id.c_str());
}
    */

// AAP factory members
void
jalv_worker_init(LV2ImplWorldContext*                       ZIX_UNUSED(jalv),
                 JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded);
LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data);
void
jalv_worker_destroy(JalvWorker* worker);
void
jalv_worker_finish(JalvWorker* worker);

LilvInstance* instantiate_plugin(
        LV2ImplWorldContext* worldContext,
        LV2ImplPluginContext* pluginContext,
        const LilvPlugin* plugin,
        int sampleRate,
        bool offlineMode) {
    auto ctx = pluginContext;

    if (zix_sem_init(&ctx->work_lock, 1)) {
        worldContext->logger->logError("Failed to initialize semaphore (work_lock)");
        return nullptr;
    }
    ctx->worker.ctx = ctx;
    ctx->state_worker.ctx = ctx;

    auto& features = worldContext->features;
    features.worker_schedule_data.handle = &ctx->worker;
    features.worker_schedule_data.schedule_work = jalv_worker_schedule;
    features.state_worker_schedule_data.handle = &ctx->state_worker;
    features.state_worker_schedule_data.schedule_work = jalv_worker_schedule;

    // for jalv worker
    if (zix_sem_init(&ctx->worker.sem, 0)) {
        worldContext->logger->logError("Failed to initialize semaphore on worker.");
        return nullptr;
    }

    /* Check for thread-safe state restore() method. */
    LilvNode* state_threadSafeRestore = lilv_new_uri(
            ctx->world, LV2_STATE__threadSafeRestore);
    if (lilv_plugin_has_feature(ctx->plugin, state_threadSafeRestore)) {
        ctx->safe_restore = true;
    }
    lilv_node_free(state_threadSafeRestore);

    ctx->instance = lilv_plugin_instantiate(plugin, sampleRate, worldContext->features.features);
    if (!ctx->instance) {
        worldContext->logger->logError("Failed to instantiate LV2 plugin.");
        return nullptr;
    }
    if (lilv_plugin_has_extension_data(ctx->plugin, ctx->statics->work_interface_uri_node)) {
        const auto* iface = (const LV2_Worker_Interface*)
                lilv_instance_get_extension_data(ctx->instance, LV2_WORKER__interface);

        jalv_worker_init(worldContext, &ctx->worker, iface, true);
        if (ctx->safe_restore) {
            jalv_worker_init(worldContext, &ctx->state_worker, iface, false);
        }
    }

    // set offline mode
    if (offlineMode) {
        auto iface = (LV2_Options_Interface*) lilv_instance_get_extension_data(ctx->instance, LV2_OPTIONS__options);
        LV2_Options_Option opts[] = {
            {LV2_OPTIONS_INSTANCE, 0, ctx->statics->urids.urid_core_free_wheeling, sizeof(bool), ctx->statics->urids.urid_atom_bool_type, &offlineMode},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0}
        };
        auto result = iface->set(ctx->instance, opts);
        if (result != LV2_OPTIONS_SUCCESS)
            ctx->statics->logger->logWarning("Failed to set offlineMode (most likely ignored)");
    }


    return ctx->instance;
}

void on_terminate_plugin(LV2ImplPluginContext* l) {

    l->exit = true;

    // Terminate the worker
    jalv_worker_finish(&l->worker);

    // Destroy the worker
    jalv_worker_destroy(&l->worker);

    lilv_instance_free(l->instance);
    delete l->statics;
    lilv_world_free(l->world);
    delete l;
}

}
