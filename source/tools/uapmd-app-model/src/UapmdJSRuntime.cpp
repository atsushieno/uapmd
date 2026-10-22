#include <uapmd-app-model/uapmd-app-model.hpp>
#include <remidy/detail/event-loop.hpp>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/time.h>
#endif
#include <choc/javascript/choc_javascript_QuickJS.h>
#include <choc/text/choc_JSON.h>
#include <AppJsLib.h>
#include <ResEmbed/ResEmbed.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>
#include "ScriptDocumentIds.hpp"

using namespace uapmd_graph;

using namespace uapmd;

namespace uapmd_app {

struct UapmdJSRuntime::Impl {
    choc::javascript::Context context;
};

static choc::value::Value toChocValue(const AppModel::MidiClipUmpEvents& clip_events)
{
    auto result = choc::value::createObject("");
    result.setMember("tickResolution", static_cast<int32_t>(clip_events.tick_resolution));
    result.setMember("bpm", clip_events.bpm);
    auto events = choc::value::createEmptyArray();
    for (const auto& event : clip_events.events) {
        auto value = choc::value::createObject("");
        value.setMember("eventIndex", event.event_index);
        value.setMember("tick", choc::value::createInt64(static_cast<int64_t>(event.tick)));
        auto words = choc::value::createEmptyArray();
        for (auto word : event.words)
            words.addArrayElement(choc::value::createInt64(static_cast<int64_t>(word)));
        value.setMember("words", words);
        events.addArrayElement(value);
    }
    result.setMember("events", events);
    return result;
}

UapmdJSRuntime::UapmdJSRuntime()
    : impl_(std::make_unique<Impl>())
{
    reinitialize();
    registerAllParameterListeners();
    registerAllMetadataListeners();
}

UapmdJSRuntime::~UapmdJSRuntime() = default;

std::shared_ptr<UapmdJSRuntime::MutationJob> UapmdJSRuntime::createMutationJob()
{
    auto job = std::make_shared<MutationJob>();
    job->id = next_mutation_job_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutation_jobs_mutex_);
    mutation_jobs_[job->id] = job;
    return job;
}

auto UapmdJSRuntime::mutationJobValue(
    const std::shared_ptr<MutationJob>& job)
{
    auto result = choc::value::createObject("MutationJob");
    if (!job)
        return result;
    std::lock_guard lock(job->mutex);
    result.setMember("jobId", job->id);
    result.setMember("state", job->state);
    result.setMember("error", job->error);
    result.setMember("result", choc::json::parse(job->resultJson));
    return result;
}

template <typename Value>
void UapmdJSRuntime::completeMutationJob(
    const std::shared_ptr<MutationJob>& job,
    Value result,
    std::string error)
{
    if (!job)
        return;
    std::lock_guard lock(job->mutex);
    job->resultJson = choc::json::toString(result);
    job->error = std::move(error);
    job->state = job->error.empty() ? "completed" : "failed";
}

void UapmdJSRuntime::reinitialize()
{
    impl_->context = choc::javascript::createQuickJSContext();
    apiBootstrapped_ = false;

    registerConsoleFunctions();
    registerProjectAPI();
    registerPluginCatalogAPI();
    registerPluginScanToolAPI();
    registerPluginInstanceAPI();
    registerSequencerMidiAPI();
    registerSequencerTransportAPI();
    registerSequencerInstanceAPI();
    registerSequencerAudioAnalysisAPI();
    registerSequencerAudioDeviceAPI();
    registerTimelineAPI();
    registerSelectionAPI();
    registerRenderAPI();
}

void UapmdJSRuntime::ensureApiBootstrapped()
{
    if (apiBootstrapped_)
        return;

    auto data = ResEmbed::get ("uapmd-api.js", "AppJsLib");
    if (! data)
        throw std::runtime_error ("Embedded AppJsLib/uapmd-api.js was not found");

    std::string src (reinterpret_cast<const char*> (data.data()), data.size());
    impl_->context.evaluateExpression (src);
    apiBootstrapped_ = true;
}

std::string UapmdJSRuntime::evaluateScript(const std::string& code,
    std::function<std::optional<std::string>(std::string_view)> fallbackModuleResolver)
{
    if (code.find ("import") == std::string::npos)
    {
        auto result = impl_->context.evaluateExpression (code);
        return result.isVoid() ? std::string ("undefined") : choc::json::toString (result);
    }

    std::string error;
    std::string output ("undefined");
    impl_->context.runModule (code,
        [&fallbackModuleResolver] (std::string_view modulePath) -> std::optional<std::string>
        {
            auto name = std::string (modulePath);
            auto withExt = name.ends_with (".js") ? name : name + ".js";
            if (auto data = ResEmbed::get (withExt, "AppJsLib"))
                return std::string (reinterpret_cast<const char*> (data.data()), data.size());
            if (fallbackModuleResolver)
                return fallbackModuleResolver (modulePath);
            return std::nullopt;
        },
        [&error, &output] (const std::string& err, const choc::value::ValueView& result)
        {
            if (! err.empty())
                error = err;
            else if (! result.isVoid())
                output = choc::json::toString (result);
        });
    if (! error.empty())
        throw std::runtime_error (error);
    return output;
}

void UapmdJSRuntime::registerConsoleFunctions()
{
    // Register console.log for debugging
    impl_->context.registerFunction ("log", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        std::string output;

        for (size_t i = 0; i < args.numArgs; ++i)
        {
            if (i > 0)
                output += " ";

            auto* arg = args[i];

            if (arg != nullptr)
                output += choc::json::toString (*arg, true);
        }

        // Output to stdout for now (could be directed to an ImGui log window)
        std::cout << "[JS] " << output << std::endl;
        return choc::value::Value();
    });
    impl_->context.evaluateExpression ("globalThis.console = globalThis.console || {}; globalThis.console.log = log;");
}

void UapmdJSRuntime::registerProjectAPI()
{
    impl_->context.registerFunction ("__remidy_project_save", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto filepath = args.get<std::string> (0, "");
        auto result = choc::value::createObject ("ProjectResult");
        if (filepath.empty())
        {
            result.setMember ("success", false);
            result.setMember ("error", "Project path is empty");
            return result;
        }

        auto projectResult = uapmd_app::AppModel::instance().saveProjectSync(filepath);
        result.setMember ("success", projectResult.success);
        result.setMember ("error", projectResult.error);
        return result;
    });

    impl_->context.registerFunction ("__remidy_project_load", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto filepath = args.get<std::string> (0, "");
        auto result = choc::value::createObject ("ProjectResult");
        if (filepath.empty())
        {
            result.setMember ("success", false);
            result.setMember ("error", "Project path is empty");
            return result;
        }

        auto loadPromise = std::make_shared<std::promise<uapmd_app::AppModel::ProjectResult>>();
        auto loadFuture = loadPromise->get_future();
        uapmd_app::AppModel::instance().loadProjectFromResolvedPath(filepath,
            [loadPromise](uapmd_app::AppModel::ProjectResult r) mutable {
                loadPromise->set_value(std::move(r));
            });
        // loadProjectFromResolvedPath instantiates plugins asynchronously, posting each
        // completion back to the main thread; drain the queue while waiting so the load can
        // progress instead of deadlocking against its own main-thread completions.
        while (loadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (remidy::EventLoop::runningOnMainThread())
                remidy::EventLoop::processQueuedTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto projectResult = loadFuture.get();
        result.setMember ("success", projectResult.success);
        result.setMember ("error", projectResult.error);
        return result;
    });

    impl_->context.registerFunction ("__remidy_project_load_handle", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto token = args.get<std::string> (0, "");
        auto result = choc::value::createObject ("ProjectResult");
        if (token.empty())
        {
            result.setMember ("success", false);
            result.setMember ("error", "Project handle token is empty");
            return result;
        }

        auto projectResult = uapmd_app::AppModel::instance().loadProjectFromHandleToken(token);
        result.setMember ("success", projectResult.success);
        result.setMember ("error", projectResult.error);
        return result;
    });
}

void UapmdJSRuntime::registerPluginCatalogAPI()
{
    impl_->context.registerFunction ("__remidy_catalog_get_count", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        const auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        const auto plugins = sequencer.engine()->pluginHost()->pluginCatalogEntries();
        return choc::value::createInt32 (static_cast<int32_t>(plugins.size()));
    });

    impl_->context.registerFunction ("__remidy_catalog_get_plugin_at", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto index = args.get<int32_t> (0, -1);
        if (index < 0)
            return choc::value::Value();

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto plugins = sequencer.engine()->pluginHost()->pluginCatalogEntries();

        if (index >= static_cast<int32_t>(plugins.size()))
            return choc::value::Value();

        auto entry = plugins[static_cast<size_t>(index)];
        if (entry.pluginId().empty())
            return choc::value::Value();

        auto obj = choc::value::createObject ("PluginCatalogEntry");
        obj.setMember ("format", entry.format());
        obj.setMember ("pluginId", entry.pluginId());
        obj.setMember ("displayName", entry.displayName());
        obj.setMember ("vendorName", entry.vendorName());
        obj.setMember ("productUrl", entry.productUrl());
        obj.setMember ("bundlePath", entry.bundlePath().string());
        return obj;
    });

    impl_->context.registerFunction ("__remidy_catalog_save", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (pathStr.empty())
            return choc::value::createBool (false);

        std::filesystem::path path (pathStr);
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->pluginHost()->savePluginCatalogToFile(path);
        return choc::value::createBool (true);
    });
}

void UapmdJSRuntime::registerPluginScanToolAPI()
{
    impl_->context.registerFunction ("__remidy_scan_tool_perform_scanning", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd_app::AppModel::instance().performPluginScanning (false);
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_scan_tool_get_formats", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& scanTool = uapmd_app::AppModel::instance().pluginScanTool();
        auto formats = scanTool.formats();

        auto arr = choc::value::createEmptyArray();
        for (auto* format : formats)
        {
            arr.addArrayElement (choc::value::createString (format->name()));
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_scan_tool_save_cache", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (! pathStr.empty())
        {
            auto& scanTool = uapmd_app::AppModel::instance().pluginScanTool();
            std::filesystem::path path (pathStr);
            scanTool.savePluginListCache (path);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_scan_tool_set_cache_file", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (! pathStr.empty())
        {
            auto& scanTool = uapmd_app::AppModel::instance().pluginScanTool();
            scanTool.pluginListCacheFile() = std::filesystem::path (pathStr);
        }
        return choc::value::Value();
    });
}

void UapmdJSRuntime::registerPluginInstanceAPI()
{
    impl_->context.registerFunction ("__remidy_instance_create", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto formatName = args.get<std::string> (0, "");
        auto pluginId = args.get<std::string> (1, "");
        auto trackId = args.get<std::string> (2, "");

        if (formatName.empty() || pluginId.empty())
            return choc::value::createInt32 (-1);

        // An empty trackId creates the instance on a new track.
        int32_t trackIndex = -1;
        if (!trackId.empty()) {
            auto resolved = script_ids::trackIndex(trackId);
            if (!resolved) {
                std::cerr << "[JS] Unknown trackId: " << trackId << std::endl;
                return choc::value::createInt32 (-1);
            }
            trackIndex = *resolved;
        }

        // Use the unified AppModel API which calls global callbacks
        // However, JavaScript needs synchronous return, so we still need to wait
        auto& model = uapmd_app::AppModel::instance();

        uapmd_app::AppModel::PluginInstanceConfig config;
        // config uses defaults: apiName="default", auto-generated device name, etc.

        std::atomic<int32_t> resultInstanceId{-1};
        std::atomic<bool> completed{false};

        // Register a temporary callback to capture the result for this specific call
        auto callback = [&resultInstanceId, &completed](const uapmd_app::AppModel::PluginInstanceResult& result) {
            if (!result.error.empty() || result.instanceId < 0) {
                std::cerr << "[JS] Failed to create plugin instance: " << result.error << std::endl;
                resultInstanceId = -1;
            } else {
                resultInstanceId = result.instanceId;
            }
            completed = true;
        };

        // Temporarily add our callback
        model.instanceCreated.push_back(callback);

        // Trigger instance creation (will call all callbacks including ours and MainWindow's)
        model.createPluginInstanceAsync(formatName, pluginId, trackIndex, config);

        // Wait for completion with timeout (max 5 seconds). This runs on the main thread,
        // and createPluginInstanceAsync delivers its completion via enqueueTaskOnMainThread
        // (from a worker thread), so we must drain the main-thread task queue while waiting;
        // a plain sleep loop would block the very thread that has to run that completion.
        auto startTime = std::chrono::steady_clock::now();
        while (!completed) {
            if (remidy::EventLoop::runningOnMainThread())
                remidy::EventLoop::processQueuedTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (std::chrono::steady_clock::now() - startTime > std::chrono::seconds(5)) {
                std::cerr << "[JS] Timeout creating plugin instance" << std::endl;
                // Remove our callback
                model.instanceCreated.pop_back();
                return choc::value::createInt32 (-1);
            }
        }

        // Remove our temporary callback
        model.instanceCreated.pop_back();

        return choc::value::createInt32 (resultInstanceId.load());
    });

    impl_->context.registerFunction ("__remidy_instance_get_parameters", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

        if (! instance)
            return choc::value::createEmptyArray();

        auto parameters = instance->parameterMetadataList();
        auto arr = choc::value::createEmptyArray();

        for (const auto& param : parameters)
        {
            auto obj = choc::value::createObject ("ParameterInfo");
            obj.setMember ("id", static_cast<int32_t>(param.index));
            obj.setMember ("name", param.name);
            obj.setMember ("minValue", param.minPlainValue);
            obj.setMember ("maxValue", param.maxPlainValue);
            obj.setMember ("defaultValue", param.defaultPlainValue);
            obj.setMember ("isAutomatable", param.automatable);
            obj.setMember ("isReadonly", false); // Assuming all are writable for now
            arr.addArrayElement (obj);
        }

        return arr;
    });

    impl_->context.registerFunction ("__remidy_instance_get_parameter_value", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto paramId = args.get<int32_t> (1, -1);

        if (instanceId < 0 || paramId < 0)
            return choc::value::createFloat64 (0.0);

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

        if (! instance)
            return choc::value::createFloat64 (0.0);

        auto value = instance->getParameterValue (paramId);
        return choc::value::createFloat64 (value);
    });

    impl_->context.registerFunction ("__remidy_instance_set_parameter_value", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto paramId = args.get<int32_t> (1, -1);
        auto value = args.get<double> (2, 0.0);

        if (instanceId < 0 || paramId < 0)
            return choc::value::Value();

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->commands().setPluginParameterValue(
            instanceId, paramId, value);

        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_dispose", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId >= 0)
        {
            uapmd_app::AppModel::instance().removePluginInstance (instanceId);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_configure", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        // Placeholder for configuration
        return choc::value::createBool (true);
    });

    impl_->context.registerFunction ("__remidy_instance_start_processing", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        // Placeholder
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_stop_processing", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        // Placeholder
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_enable_ump_device", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto deviceName = args.get<std::string> (1, "");

        if (instanceId >= 0)
        {
            uapmd_app::AppModel::instance().enableUmpDevice (instanceId, deviceName);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_disable_ump_device", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);

        if (instanceId >= 0)
        {
            uapmd_app::AppModel::instance().disableUmpDevice (instanceId);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_show_ui", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);

        if (instanceId >= 0)
        {
            uapmd_app::AppModel::instance().requestShowPluginUI (instanceId);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_hide_ui", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);

        if (instanceId >= 0)
        {
            uapmd_app::AppModel::instance().hidePluginUI (instanceId);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_show_details", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId >= 0)
            uapmd_app::AppModel::instance().requestShowInstanceDetails(instanceId);
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_instance_save_state", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto filepath = args.get<std::string> (1, "");

        if (instanceId < 0 || filepath.empty())
        {
            auto result = choc::value::createObject ("PluginStateResult");
            result.setMember ("success", false);
            result.setMember ("error", "Invalid instance ID or filepath");
            return result;
        }

        auto result = uapmd_app::AppModel::instance().savePluginStateSync (instanceId, filepath);

        auto obj = choc::value::createObject ("PluginStateResult");
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        obj.setMember ("filepath", result.filepath);
        obj.setMember ("instanceId", result.instanceId);
        return obj;
    });

    impl_->context.registerFunction ("__remidy_instance_load_state", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto filepath = args.get<std::string> (1, "");

        if (instanceId < 0 || filepath.empty())
        {
            auto result = choc::value::createObject ("PluginStateResult");
            result.setMember ("success", false);
            result.setMember ("error", "Invalid instance ID or filepath");
            return result;
        }

        auto result = uapmd_app::AppModel::instance().loadPluginStateSync (instanceId, filepath);

        auto obj = choc::value::createObject ("PluginStateResult");
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        obj.setMember ("filepath", result.filepath);
        obj.setMember ("instanceId", result.instanceId);
        return obj;
    });

    impl_->context.registerFunction ("__remidy_instance_get_presets", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

        if (! instance)
            return choc::value::createEmptyArray();

        auto presets = instance->presetMetadataList();
        auto arr = choc::value::createEmptyArray();

        for (const auto& p : presets)
        {
            auto obj = choc::value::createObject ("PresetInfo");
            obj.setMember ("index", static_cast<int32_t>(p.index));
            obj.setMember ("name", p.name);
            obj.setMember ("bank", static_cast<int32_t>(p.bank));
            arr.addArrayElement (obj);
        }

        return arr;
    });

    impl_->context.registerFunction ("__remidy_instance_load_preset", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto presetIndex = args.get<int32_t> (1, -1);
        auto job = createMutationJob();

        if (instanceId < 0 || presetIndex < 0) {
            completeMutationJob(
                job,
                choc::value::createObject("PluginPresetResult"),
                "Invalid instance ID or preset index");
            return mutationJobValue(job);
        }

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->timeline().loadPluginPreset(
            instanceId,
            presetIndex,
            uapmd::ProjectMutationOrigin::Remote,
            [this, job](uapmd::ProjectUndoResult result) mutable {
                auto value = choc::value::createObject("PluginPresetResult");
                value.setMember("success", result.succeeded());
                completeMutationJob(job, std::move(value), std::move(result.error));
            });
        return mutationJobValue(job);
    });
}

void UapmdJSRuntime::registerSequencerMidiAPI()
{
    impl_->context.registerFunction ("__remidy_sequencer_sendNoteOn", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto note = args.get<int32_t> (1, 60);

        if (instanceId >= 0 && note >= 0 && note < 128)
        {
            auto& sequencer = uapmd_app::AppModel::instance().sequencer();
            sequencer.engine()->sendNoteOn (instanceId, note);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_sendNoteOff", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto note = args.get<int32_t> (1, 60);

        if (instanceId >= 0 && note >= 0 && note < 128)
        {
            auto& sequencer = uapmd_app::AppModel::instance().sequencer();
            sequencer.engine()->sendNoteOff (instanceId, note);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_setParameterValue", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto paramIndex = args.get<int32_t> (1, -1);
        auto value = args.get<double> (2, 0.0);

        if (instanceId >= 0 && paramIndex >= 0)
        {
            auto& sequencer = uapmd_app::AppModel::instance().sequencer();
            sequencer.engine()->commands().setPluginParameterValue(
                instanceId, paramIndex, value);
        }
        return choc::value::Value();
    });
}

void UapmdJSRuntime::registerSequencerTransportAPI()
{
    impl_->context.registerFunction ("__remidy_sequencer_startPlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->startPlayback();
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_stopPlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->stopPlayback();
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_pausePlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->pausePlayback();
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_resumePlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        sequencer.engine()->resumePlayback();
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_getPlaybackPosition", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto position = sequencer.engine()->playbackPosition();
        return choc::value::createInt64 (position);
    });

    impl_->context.registerFunction ("__remidy_sequencer_jumpPlayback", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        uapmd_app::AppModel::instance().transport().jump (args.get<double> (0, std::numeric_limits<double>::quiet_NaN()));
        return choc::value::Value();
    });
}

void UapmdJSRuntime::registerSequencerInstanceAPI()
{
    impl_->context.registerFunction ("__remidy_sequencer_getInstanceIds", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto instanceIds = sequencer.engine()->pluginHost()->instanceIds();

        auto arr = choc::value::createEmptyArray();
        for (auto id : instanceIds)
        {
            arr.addArrayElement (choc::value::createInt32 (id));
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_sequencer_getPluginName", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createString ("");

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto instance = sequencer.engine()->getPluginInstance (instanceId);
        return choc::value::createString (instance->displayName());
    });

    impl_->context.registerFunction ("__remidy_sequencer_getPluginFormat", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createString ("");

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto format = sequencer.getPluginFormat (instanceId);
        return choc::value::createString (format);
    });

    impl_->context.registerFunction ("__remidy_sequencer_isPluginBypassed", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createBool (false);

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);
        return choc::value::createBool (instance ? instance->bypassed() : false);
    });

    impl_->context.registerFunction ("__remidy_sequencer_setPluginBypassed", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto bypassed = args.get<bool> (1, false);

        if (instanceId >= 0)
        {
            auto& sequencer = uapmd_app::AppModel::instance().sequencer();
            sequencer.engine()->commands().setPluginBypassed(
                instanceId, bypassed);
        }
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_sequencer_get_history_state", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        const auto state = uapmd_app::AppModel::instance().historyState();
        auto result = choc::value::createObject("HistoryState");
        result.setMember("busy", state.busy);
        result.setMember("compoundOpen", state.compoundOpen);
        result.setMember("gestureOpen", state.gestureOpen);
        result.setMember("canUndo", state.canUndo);
        result.setMember("canRedo", state.canRedo);
        result.setMember("dirty", state.dirty);
        result.setMember("compoundDescription", state.compoundDescription);
        result.setMember("undoDescription", state.undoDescription);
        result.setMember("redoDescription", state.redoDescription);
        result.setMember("historySizeInBytes", choc::value::createInt64(
            static_cast<int64_t>(state.historySizeInBytes)));
        result.setMember("maximumHistorySizeInBytes", choc::value::createInt64(
            static_cast<int64_t>(state.maximumHistorySizeInBytes)));
        result.setMember("currentStateId", choc::value::createInt64(
            static_cast<int64_t>(state.currentStateId)));
        result.setMember("savedStateId", choc::value::createInt64(
            static_cast<int64_t>(state.savedStateId)));
        return result;
    });

    impl_->context.registerFunction ("__remidy_sequencer_begin_compound", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto description = args.get<std::string> (0, "");
        auto& undo = uapmd_app::AppModel::instance().sequencer().engine()->commands().history();
        const auto result = undo.beginStep(description, uapmd::ProjectMutationOrigin::Remote);
        auto value = choc::value::createObject("HistoryScopeResult");
        value.setMember("success", result.succeeded());
        value.setMember("error", result.error);
        return value;
    });

    impl_->context.registerFunction ("__remidy_sequencer_end_compound", [this] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto job = createMutationJob();
        auto& undo = uapmd_app::AppModel::instance().sequencer().engine()->commands().history();
        undo.endStep([this, job](uapmd::ProjectUndoResult result) mutable {
            auto value = choc::value::createObject("HistoryMutationResult");
            value.setMember("success", result.succeeded());
            completeMutationJob(job, std::move(value), std::move(result.error));
        });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_cancel_compound", [this] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto job = createMutationJob();
        auto& undo = uapmd_app::AppModel::instance().sequencer().engine()->commands().history();
        undo.cancelStep([this, job](uapmd::ProjectUndoResult result) mutable {
            auto value = choc::value::createObject("HistoryMutationResult");
            value.setMember("success", result.succeeded());
            completeMutationJob(job, std::move(value), std::move(result.error));
        });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_undo", [this] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto job = createMutationJob();
        uapmd_app::AppModel::instance().undo(
            [this, job](std::string error) mutable {
                auto result = choc::value::createObject("HistoryMutationResult");
                result.setMember("success", error.empty());
                completeMutationJob(job, std::move(result), std::move(error));
            });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_redo", [this] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto job = createMutationJob();
        uapmd_app::AppModel::instance().redo(
            [this, job](std::string error) mutable {
                auto result = choc::value::createObject("HistoryMutationResult");
                result.setMember("success", error.empty());
                completeMutationJob(job, std::move(result), std::move(error));
            });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_getTrackInfos", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto tracks = sequencer.engine()->tracks();

        auto arr = choc::value::createEmptyArray();
        for (uapmd_track_index_t i = 0, n = tracks.size(); i < n; ++i) {
            auto track = tracks[i];
            auto trackObj = choc::value::createObject ("TrackInfo");
            trackObj.setMember ("trackId", script_ids::trackId(i));

            auto nodesArr = choc::value::createEmptyArray();
            for (const auto& [nodeId, graphNode] : track->graph().nodes()) {
                if (!graphNode)
                    continue;
                auto nodeObj = choc::value::createObject ("GraphNodeInfo");
                nodeObj.setMember ("nodeId", nodeId);
                nodeObj.setMember ("nodeType", graphNode->nodeType());
                nodeObj.setMember ("displayName", graphNode->displayName());
                if (auto* pluginNode = dynamic_cast<AudioPluginNode*>(graphNode)) {
                    auto* instance = pluginNode->instance();
                    nodeObj.setMember ("isPlugin", true);
                    nodeObj.setMember ("instanceId", pluginNode->instanceId());
                    if (instance) {
                        nodeObj.setMember ("pluginId", instance->pluginId());
                        nodeObj.setMember ("format", instance->formatName());
                    } else {
                        nodeObj.setMember ("pluginId", std::string{});
                        nodeObj.setMember ("format", std::string{});
                    }
                } else {
                    nodeObj.setMember ("isPlugin", false);
                    nodeObj.setMember ("instanceId", -1);
                    nodeObj.setMember ("pluginId", std::string{});
                    nodeObj.setMember ("format", std::string{});
                }
                nodesArr.addArrayElement (nodeObj);
            }
            trackObj.setMember ("nodes", nodesArr);
            arr.addArrayElement (trackObj);
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_mutation_job_get", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto jobId = args.get<int32_t> (0, -1);
        std::shared_ptr<MutationJob> job;
        {
            std::lock_guard lock(mutation_jobs_mutex_);
            const auto it = mutation_jobs_.find(jobId);
            if (it != mutation_jobs_.end())
                job = it->second;
        }
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_mutation_job_clear", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto jobId = args.get<int32_t> (0, -1);
        bool removed = false;
        {
            std::lock_guard lock(mutation_jobs_mutex_);
            removed = mutation_jobs_.erase(jobId) != 0;
        }
        return choc::value::createBool(removed);
    });

    impl_->context.registerFunction ("__remidy_sequencer_add_track", [this] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto job = createMutationJob();
        uapmd_app::AppModel::instance().addTrack(
            [this, job](int32_t trackIndex, std::string error) {
                auto result = choc::value::createObject("TrackMutationResult");
                result.setMember("trackId", trackIndex < 0 ? std::string{} : script_ids::trackId(trackIndex));
                completeMutationJob(job, std::move(result), std::move(error));
            });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_remove_track", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto trackId = args.get<std::string> (0, "");
        const auto trackIndex = script_ids::trackIndex(trackId);
        auto job = createMutationJob();
        if (!trackIndex || *trackIndex < 0) {
            completeMutationJob(
                job,
                choc::value::createObject("TrackMutationResult"),
                "Invalid trackId");
            return mutationJobValue(job);
        }
        uapmd_app::AppModel::instance().removeTrack(
            *trackIndex,
            [this, job, trackId](int32_t, std::string error) {
                auto result = choc::value::createObject("TrackMutationResult");
                result.setMember("trackId", trackId);
                completeMutationJob(job, std::move(result), std::move(error));
            });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_clear_tracks", [this] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto job = createMutationJob();
        uapmd_app::AppModel::instance().removeAllTracks(
            [this, job](std::string error) {
                auto result = choc::value::createObject("TrackClearResult");
                result.setMember("success", error.empty());
                completeMutationJob(job, std::move(result), std::move(error));
            });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_sequencer_getParameterUpdates", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        std::vector<ParameterUpdate> updates;
        {
            std::lock_guard<std::mutex> lock(js_parameter_mutex_);
            auto it = js_parameter_updates_.find(instanceId);
            if (it != js_parameter_updates_.end()) {
                updates = std::move(it->second);
                js_parameter_updates_.erase(it);
            }
        }

        auto arr = choc::value::createEmptyArray();
        for (const auto& update : updates)
        {
            auto obj = choc::value::createObject ("ParameterUpdate");
            obj.setMember ("parameterIndex", update.parameterIndex);
            obj.setMember ("value", update.value);
            arr.addArrayElement (obj);
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_sequencer_consumeParameterMetadataRefresh", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        bool refreshed = false;
        if (instanceId >= 0)
        {
            std::lock_guard<std::mutex> lock(js_metadata_mutex_);
            auto it = js_metadata_refresh_.find(instanceId);
            if (it != js_metadata_refresh_.end()) {
                js_metadata_refresh_.erase(it);
                refreshed = true;
            }
        }
        return choc::value::createBool (refreshed);
    });
}

void UapmdJSRuntime::registerSequencerAudioAnalysisAPI()
{
    impl_->context.registerFunction ("__remidy_sequencer_getInputTimeDomainData", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto numBars = args.get<int32_t> (0, 32);
        if (numBars <= 0 || numBars > 256)
            numBars = 32;

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        std::vector<float> values (static_cast<size_t>(numBars));
        if (auto* analyser = sequencer.engine()->inputAnalyser())
            analyser->getFloatTimeDomainData(values.data(), static_cast<uint32_t>(numBars));

        auto arr = choc::value::createEmptyArray();
        for (auto value : values)
        {
            arr.addArrayElement (choc::value::createFloat32 (value));
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_sequencer_getOutputTimeDomainData", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto numBars = args.get<int32_t> (0, 32);
        if (numBars <= 0 || numBars > 256)
            numBars = 32;

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        std::vector<float> values (static_cast<size_t>(numBars));
        if (auto* analyser = sequencer.engine()->outputAnalyser())
            analyser->getFloatTimeDomainData(values.data(), static_cast<uint32_t>(numBars));

        auto arr = choc::value::createEmptyArray();
        for (auto value : values)
        {
            arr.addArrayElement (choc::value::createFloat32 (value));
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_sequencer_getInputFrequencyData", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto binCount = args.get<int32_t> (0, 128);
        if (binCount <= 0 || binCount > 2048)
            binCount = 128;
        std::vector<float> values (static_cast<size_t>(binCount));
        if (auto* analyser = uapmd_app::AppModel::instance().sequencer().engine()->inputAnalyser())
            analyser->getFloatFrequencyData(values.data(), static_cast<uint32_t>(binCount));
        auto arr = choc::value::createEmptyArray();
        for (auto value : values)
            arr.addArrayElement(choc::value::createFloat32(value));
        return arr;
    });

    impl_->context.registerFunction ("__remidy_sequencer_getOutputFrequencyData", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto binCount = args.get<int32_t> (0, 128);
        if (binCount <= 0 || binCount > 2048)
            binCount = 128;
        std::vector<float> values (static_cast<size_t>(binCount));
        if (auto* analyser = uapmd_app::AppModel::instance().sequencer().engine()->outputAnalyser())
            analyser->getFloatFrequencyData(values.data(), static_cast<uint32_t>(binCount));
        auto arr = choc::value::createEmptyArray();
        for (auto value : values)
            arr.addArrayElement(choc::value::createFloat32(value));
        return arr;
    });
}

void UapmdJSRuntime::registerSequencerAudioDeviceAPI()
{
    impl_->context.registerFunction("__remidy_audio_worker_diagnostics", [](choc::javascript::ArgumentList) {
        auto* engine = AppModel::instance().sequencer().engine();
        const auto d = engine->audioWorkers().diagnostic();
        auto result = choc::value::createObject("");
        result.setMember("configuredWorkers", static_cast<int32_t>(engine->audioWorkers().count()));
        result.setMember("currentFault", static_cast<int32_t>(engine->audioWorkers().fault()));
        result.setMember("stopOnDeadline", engine->audioWorkers().stopOnDeadline());
        const auto counters = engine->audioPerformanceCounter().counters();
        auto timing = choc::value::createObject("");
        timing.setMember("realtimeBlocks", static_cast<int64_t>(counters.realtime_blocks));
        timing.setMember("deadlineMisses", static_cast<int64_t>(counters.deadline_misses));
        timing.setMember("droppedRecords", static_cast<int64_t>(counters.dropped_records));
        result.setMember("timingCounters", timing);
        result.setMember("fault", static_cast<int32_t>(d.fault));
        result.setMember("block", static_cast<int64_t>(d.block_number));
        result.setMember("workers", static_cast<int32_t>(d.worker_count));
        result.setMember("frames", d.frame_count);
        result.setMember("sampleRate", d.sample_rate);
        result.setMember("playbackSamples", d.playback_position_samples);
        result.setMember("callbackElapsedMs", d.elapsed_ms);
        result.setMember("dispatchAtMs", d.dispatch_ms);
        result.setMember("offline", d.offline);
        result.setMember("engineStopped", d.engine_stopped);
        const auto serialize = [&](const uapmd::AudioWorkerProgressSnapshot& snapshot) {
            auto value = choc::value::createObject("");
            value.setMember("trackCount", static_cast<int32_t>(snapshot.track_count));
            value.setMember("completedJobs", static_cast<int32_t>(snapshot.completed_jobs));
            value.setMember("pendingParticipants", static_cast<int32_t>(snapshot.pending_participants));
            auto participants = choc::value::createEmptyArray();
            for (uint32_t i = 0; i <= d.worker_count; ++i) {
                const auto& p = snapshot.participants[i];
                auto item = choc::value::createObject("");
                item.setMember("participant", static_cast<int32_t>(i));
                item.setMember("acknowledgedNs", static_cast<int64_t>(p.acknowledged_ns));
                item.setMember("retiredNs", static_cast<int64_t>(p.retired_ns));
                item.setMember("currentTrack", p.current_track);
                item.setMember("completedJobs", static_cast<int32_t>(p.completed_jobs));
                participants.addArrayElement(item);
            }
            value.setMember("participants", participants);
            auto tracks = choc::value::createEmptyArray();
            for (size_t i = 0; i < snapshot.tracks.size() && i < snapshot.track_count; ++i) {
                const auto& t = snapshot.tracks[i];
                auto item = choc::value::createObject("");
                item.setMember("track", static_cast<int32_t>(i));
                item.setMember("participant", t.participant);
                item.setMember("startedNs", static_cast<int64_t>(t.started_ns));
                item.setMember("finishedNs", static_cast<int64_t>(t.finished_ns));
                item.setMember("cpuNs", static_cast<int64_t>(t.cpu_ns));
                item.setMember("status", t.status);
                tracks.addArrayElement(item);
            }
            value.setMember("tracks", tracks);
            return value;
        };
        result.setMember("atFault", serialize(d.at_fault));
        result.setMember("completionAvailable", d.completion_available);
        if (d.completion_available)
            result.setMember("afterCompletion", serialize(d.after_completion));
        return result;
    });
    impl_->context.registerFunction("__remidy_configure_audio_workers", [](choc::javascript::ArgumentList args) {
        const auto count = args.get<int32_t>(0, -1);
        return choc::value::createBool(count >= 0 && count <= 32 &&
            AppModel::instance().sequencer().engine()->audioWorkers().configure(static_cast<uint32_t>(count)));
    });
    impl_->context.registerFunction("__remidy_set_stop_on_audio_worker_deadline", [](choc::javascript::ArgumentList args) {
        AppModel::instance().sequencer().engine()->audioWorkers().setStopOnDeadline(args.get<bool>(0, false));
        return choc::value::createBool(true);
    });

    impl_->context.registerFunction ("__remidy_sequencer_getSampleRate", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto sampleRate = sequencer.sampleRate();
        return choc::value::createInt32 (sampleRate);
    });

    impl_->context.registerFunction ("__remidy_sequencer_setSampleRate", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto sampleRate = args.get<int32_t> (0, 48000);
        if (sampleRate <= 0)
            return choc::value::createBool (false);

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        auto success = sequencer.sampleRate (sampleRate);
        return choc::value::createBool (success);
    });

    impl_->context.registerFunction ("__remidy_sequencer_reconfigureAudioDevice", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto inputIndex = args.get<int32_t> (0, -1);
        auto outputIndex = args.get<int32_t> (1, -1);
        auto sampleRate = args.get<int32_t> (2, 0);
        auto bufferSize = args.get<int32_t> (3, 0);
        if (sampleRate < 0 || bufferSize < 0)
            return choc::value::createBool (false);

        auto& sequencer = uapmd_app::AppModel::instance().sequencer();
        if (!sequencer.reconfigureAudioDevice (inputIndex, outputIndex,
                                               static_cast<uint32_t> (sampleRate),
                                               static_cast<uint32_t> (bufferSize)))
            return choc::value::createBool (false);
        uapmd_app::AppModel::instance().updateAudioDeviceSettings (sequencer.sampleRate(), static_cast<uint32_t> (bufferSize));
        return choc::value::createBool (true);
    });

    impl_->context.registerFunction ("__remidy_sequencer_isScanning", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto isScanning = uapmd_app::AppModel::instance().isScanning();
        return choc::value::createBool (isScanning);
    });

    // Audio engine on/off. Disabling it frees the realtime render thread, which is
    // essential for heavy operations (e.g. loading a large project with many plugins)
    // that would otherwise compete with the live engine for CPU.
    impl_->context.registerFunction ("__remidy_set_audio_engine_enabled", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto enabled = args.get<bool> (0, true);
        uapmd_app::AppModel::instance().setAudioEngineEnabled (enabled);
        return choc::value::createBool (uapmd_app::AppModel::instance().isAudioEngineEnabled());
    });

    impl_->context.registerFunction ("__remidy_is_audio_engine_enabled", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        return choc::value::createBool (uapmd_app::AppModel::instance().isAudioEngineEnabled());
    });
}

void UapmdJSRuntime::registerParameterListener(int32_t instanceId)
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        auto node = track->graph().getPluginNode(instanceId);
        if (node) {
            auto listenerId = node->parameterUpdateEvent().addListener([this, instanceId](int32_t paramIndex, double value) {
                std::lock_guard<std::mutex> lock(js_parameter_mutex_);
                js_parameter_updates_[instanceId].push_back({paramIndex, value});
            });
            std::lock_guard<std::mutex> lock(js_parameter_mutex_);
            js_parameter_listener_ids_[instanceId] = listenerId;
            break;
        }
    }
}

void UapmdJSRuntime::unregisterParameterListener(int32_t instanceId)
{
    EventListenerId listenerId = 0;
    {
        std::lock_guard<std::mutex> lock(js_parameter_mutex_);
        auto it = js_parameter_listener_ids_.find(instanceId);
        if (it != js_parameter_listener_ids_.end()) {
            listenerId = it->second;
            js_parameter_listener_ids_.erase(it);
        }
        js_parameter_updates_.erase(instanceId);
    }

    if (listenerId != 0) {
        auto& seq = AppModel::instance().sequencer();
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterUpdateEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

void UapmdJSRuntime::registerAllParameterListeners()
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        for (const auto& [instanceId, node] : track->graph().plugins()) {
            if (node) {
                // Only register if not already registered
                std::lock_guard<std::mutex> lock(js_parameter_mutex_);
                if (js_parameter_listener_ids_.find(instanceId) == js_parameter_listener_ids_.end()) {
                    auto listenerId = node->parameterUpdateEvent().addListener([this, instanceId](int32_t paramIndex, double value) {
                        std::lock_guard<std::mutex> lock(js_parameter_mutex_);
                        js_parameter_updates_[instanceId].push_back({paramIndex, value});
                    });
                    js_parameter_listener_ids_[instanceId] = listenerId;
                }
            }
        }
    }
}

void UapmdJSRuntime::unregisterAllParameterListeners()
{
    std::map<int32_t, EventListenerId> listenerIdsCopy;
    {
        std::lock_guard<std::mutex> lock(js_parameter_mutex_);
        listenerIdsCopy = js_parameter_listener_ids_;
        js_parameter_listener_ids_.clear();
        js_parameter_updates_.clear();
    }

    auto& seq = AppModel::instance().sequencer();
    for (const auto& [instanceId, listenerId] : listenerIdsCopy) {
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterUpdateEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

void UapmdJSRuntime::registerMetadataListener(int32_t instanceId)
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        auto node = track->graph().getPluginNode(instanceId);
        if (node) {
            auto listenerId = node->parameterMetadataRefreshEvent().addListener([this, instanceId]() {
                std::lock_guard<std::mutex> lock(js_metadata_mutex_);
                js_metadata_refresh_.insert(instanceId);
            });
            std::lock_guard<std::mutex> lock(js_metadata_mutex_);
            js_metadata_listener_ids_[instanceId] = listenerId;
            break;
        }
    }
}

void UapmdJSRuntime::unregisterMetadataListener(int32_t instanceId)
{
    EventListenerId listenerId = 0;
    {
        std::lock_guard<std::mutex> lock(js_metadata_mutex_);
        auto it = js_metadata_listener_ids_.find(instanceId);
        if (it != js_metadata_listener_ids_.end()) {
            listenerId = it->second;
            js_metadata_listener_ids_.erase(it);
        }
        js_metadata_refresh_.erase(instanceId);
    }

    if (listenerId != 0) {
        auto& seq = AppModel::instance().sequencer();
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterMetadataRefreshEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

void UapmdJSRuntime::registerAllMetadataListeners()
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        for (const auto& [instanceId, node] : track->graph().plugins()) {
            if (node) {
                // Only register if not already registered
                std::lock_guard<std::mutex> lock(js_metadata_mutex_);
                if (js_metadata_listener_ids_.find(instanceId) == js_metadata_listener_ids_.end()) {
                    auto listenerId = node->parameterMetadataRefreshEvent().addListener([this, instanceId]() {
                        std::lock_guard<std::mutex> lock(js_metadata_mutex_);
                        js_metadata_refresh_.insert(instanceId);
                    });
                    js_metadata_listener_ids_[instanceId] = listenerId;
                }
            }
        }
    }
}

void UapmdJSRuntime::unregisterAllMetadataListeners()
{
    std::map<int32_t, EventListenerId> listenerIdsCopy;
    {
        std::lock_guard<std::mutex> lock(js_metadata_mutex_);
        listenerIdsCopy = js_metadata_listener_ids_;
        js_metadata_listener_ids_.clear();
        js_metadata_refresh_.clear();
    }

    auto& seq = AppModel::instance().sequencer();
    for (const auto& [instanceId, listenerId] : listenerIdsCopy) {
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterMetadataRefreshEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

namespace {

constexpr std::string_view kMasterMarkerReferenceId = "master_track";

std::string timeReferenceTypeToString(uapmd::TimeReferenceType type) {
    switch (type) {
        case uapmd::TimeReferenceType::ContainerStart: return "containerStart";
        case uapmd::TimeReferenceType::ContainerEnd: return "containerEnd";
        case uapmd::TimeReferenceType::Point: return "point";
    }
    return "containerStart";
}

bool parseTimeReferenceType(std::string_view text, uapmd::TimeReferenceType& type) {
    if (text == "containerStart") {
        type = uapmd::TimeReferenceType::ContainerStart;
        return true;
    }
    if (text == "containerEnd") {
        type = uapmd::TimeReferenceType::ContainerEnd;
        return true;
    }
    if (text == "point") {
        type = uapmd::TimeReferenceType::Point;
        return true;
    }
    return false;
}

choc::value::Value serializeTimeReference(const uapmd::TimeReference& reference) {
    auto obj = choc::value::createObject("TimeReference");
    obj.setMember("type", timeReferenceTypeToString(reference.type));
    obj.setMember("referenceId", reference.referenceId);
    obj.setMember("offset", reference.offset);
    return obj;
}

choc::value::Value serializeMarker(const uapmd::ClipMarker& marker, std::string_view ownerReferenceId) {
    auto obj = choc::value::createObject("ClipMarker");
    obj.setMember("markerId", marker.markerId);
    obj.setMember("name", marker.name);
    obj.setMember("timeReference", serializeTimeReference(marker.timeReference(ownerReferenceId, kMasterMarkerReferenceId)));
    return obj;
}

choc::value::Value serializeWarp(const uapmd::AudioWarpPoint& warp, std::string_view ownerReferenceId) {
    auto obj = choc::value::createObject("AudioWarpPoint");
    obj.setMember("speedRatio", warp.speedRatio);
    obj.setMember("timeReference", serializeTimeReference(warp.timeReference(ownerReferenceId, kMasterMarkerReferenceId)));
    return obj;
}

bool parseTimeReferenceValue(const choc::value::ValueView& value, uapmd::TimeReference& reference, std::string& error) {
    if (!value.isObject()) {
        error = "timeReference must be an object";
        return false;
    }
    const auto typeText = value.hasObjectMember("type") ? value["type"].get<std::string>() : std::string{};
    if (!parseTimeReferenceType(typeText, reference.type)) {
        error = "timeReference.type is invalid";
        return false;
    }
    reference.referenceId = value.hasObjectMember("referenceId") ? value["referenceId"].get<std::string>() : std::string{};
    reference.offset = value.hasObjectMember("offset") ? value["offset"].get<double>() : 0.0;
    return true;
}

bool parseMarkersValue(const choc::value::ValueView* value,
                       std::string_view ownerReferenceId,
                       std::vector<uapmd::ClipMarker>& markers,
                       std::string& error) {
    markers.clear();
    if (!value)
        return true;
    if (!value->isArray()) {
        error = "markers must be an array";
        return false;
    }

    markers.reserve(value->size());
    for (uint32_t i = 0; i < value->size(); ++i) {
        const auto& item = (*value)[i];
        if (!item.isObject()) {
            error = "marker entry must be an object";
            return false;
        }
        uapmd::ClipMarker marker;
        marker.markerId = item.hasObjectMember("markerId") ? item["markerId"].get<std::string>() : std::string{};
        marker.name = item.hasObjectMember("name") ? item["name"].get<std::string>() : std::string{};
        uapmd::TimeReference reference;
        if (!item.hasObjectMember("timeReference") || !parseTimeReferenceValue(item["timeReference"], reference, error))
            return false;
        marker.setTimeReference(reference, ownerReferenceId, kMasterMarkerReferenceId);
        markers.push_back(std::move(marker));
    }
    return true;
}

bool parseWarpsValue(const choc::value::ValueView* value,
                     std::string_view ownerReferenceId,
                     std::vector<uapmd::AudioWarpPoint>& warps,
                     std::string& error) {
    warps.clear();
    if (!value)
        return true;
    if (!value->isArray()) {
        error = "audioWarps must be an array";
        return false;
    }

    warps.reserve(value->size());
    for (uint32_t i = 0; i < value->size(); ++i) {
        const auto& item = (*value)[i];
        if (!item.isObject()) {
            error = "audioWarp entry must be an object";
            return false;
        }
        uapmd::AudioWarpPoint warp;
        warp.speedRatio = item.hasObjectMember("speedRatio") ? item["speedRatio"].get<double>() : 1.0;
        uapmd::TimeReference reference;
        if (!item.hasObjectMember("timeReference") || !parseTimeReferenceValue(item["timeReference"], reference, error))
            return false;
        warp.setTimeReference(reference, ownerReferenceId, kMasterMarkerReferenceId);
        warps.push_back(std::move(warp));
    }
    return true;
}

std::string graphEndpointTypeToString(AudioPluginGraphEndpointType type) {
    switch (type) {
        case AudioPluginGraphEndpointType::GraphInput: return "graphInput";
        case AudioPluginGraphEndpointType::GraphOutput: return "graphOutput";
        case AudioPluginGraphEndpointType::Plugin: return "plugin";
    }
    return "plugin";
}

bool parseGraphEndpointType(std::string_view text, AudioPluginGraphEndpointType& type) {
    if (text == "graphInput") {
        type = AudioPluginGraphEndpointType::GraphInput;
        return true;
    }
    if (text == "graphOutput") {
        type = AudioPluginGraphEndpointType::GraphOutput;
        return true;
    }
    if (text == "plugin") {
        type = AudioPluginGraphEndpointType::Plugin;
        return true;
    }
    return false;
}

std::string graphBusTypeToString(AudioPluginGraphBusType type) {
    return type == AudioPluginGraphBusType::Event ? "event" : "audio";
}

bool parseGraphBusType(std::string_view text, AudioPluginGraphBusType& type) {
    if (text == "audio") {
        type = AudioPluginGraphBusType::Audio;
        return true;
    }
    if (text == "event") {
        type = AudioPluginGraphBusType::Event;
        return true;
    }
    return false;
}

choc::value::Value serializeGraphEndpoint(const AudioPluginGraphEndpoint& endpoint) {
    auto obj = choc::value::createObject("GraphEndpoint");
    obj.setMember("type", graphEndpointTypeToString(endpoint.type));
    if (!endpoint.node_id.empty())
        obj.setMember("nodeId", endpoint.node_id);
    obj.setMember("instanceId", endpoint.instance_id);
    obj.setMember("busIndex", static_cast<int32_t>(endpoint.bus_index));
    return obj;
}

choc::value::Value serializeGraphConnection(const AudioPluginGraphConnection& connection) {
    auto obj = choc::value::createObject("GraphConnection");
    obj.setMember("id", connection.id);
    obj.setMember("busType", graphBusTypeToString(connection.bus_type));
    obj.setMember("source", serializeGraphEndpoint(connection.source));
    obj.setMember("target", serializeGraphEndpoint(connection.target));
    return obj;
}

AudioPluginGraphEndpoint makeGraphEndpoint(
    AudioPluginGraphEndpointType type,
    std::string nodeId,
    int32_t instanceId,
    int32_t busIndex
) {
    return AudioPluginGraphEndpoint{
        .type = type,
        .node_id = std::move(nodeId),
        .instance_id = instanceId,
        .bus_index = static_cast<uint32_t>(std::max(0, busIndex)),
    };
}

std::string playbackCompensationModeToString(uapmd::PlaybackCompensationMode mode) {
    switch (mode) {
        case uapmd::PlaybackCompensationMode::COMPENSATED: return "compensated";
        case uapmd::PlaybackCompensationMode::LOW_LATENCY: return "lowLatency";
    }
    return "compensated";
}

bool parsePlaybackCompensationMode(std::string_view text, uapmd::PlaybackCompensationMode& mode) {
    if (text == "compensated") {
        mode = uapmd::PlaybackCompensationMode::COMPENSATED;
        return true;
    }
    if (text == "lowLatency") {
        mode = uapmd::PlaybackCompensationMode::LOW_LATENCY;
        return true;
    }
    return false;
}

std::string inputMonitoringPolicyToString(uapmd::InputMonitoringPolicy policy) {
    switch (policy) {
        case uapmd::InputMonitoringPolicy::TAPE_STYLE: return "tapeStyle";
        case uapmd::InputMonitoringPolicy::AUTO: return "auto";
        case uapmd::InputMonitoringPolicy::OFF: return "off";
    }
    return "auto";
}

bool parseInputMonitoringPolicy(std::string_view text, uapmd::InputMonitoringPolicy& policy) {
    if (text == "tapeStyle") {
        policy = uapmd::InputMonitoringPolicy::TAPE_STYLE;
        return true;
    }
    if (text == "auto") {
        policy = uapmd::InputMonitoringPolicy::AUTO;
        return true;
    }
    if (text == "off") {
        policy = uapmd::InputMonitoringPolicy::OFF;
        return true;
    }
    return false;
}

std::string realtimeInfiniteTailPolicyToString(uapmd::RealtimeInfiniteTailPolicy policy) {
    switch (policy) {
        case uapmd::RealtimeInfiniteTailPolicy::LATENCY_FALLBACK: return "latencyFallback";
        case uapmd::RealtimeInfiniteTailPolicy::IMMEDIATE_STOP: return "immediateStop";
    }
    return "latencyFallback";
}

bool parseRealtimeInfiniteTailPolicy(std::string_view text, uapmd::RealtimeInfiniteTailPolicy& policy) {
    if (text == "latencyFallback") {
        policy = uapmd::RealtimeInfiniteTailPolicy::LATENCY_FALLBACK;
        return true;
    }
    if (text == "immediateStop") {
        policy = uapmd::RealtimeInfiniteTailPolicy::IMMEDIATE_STOP;
        return true;
    }
    return false;
}

choc::value::Value serializeLatencyCompensationState(uapmd::LatencyCompensationManager& manager,
                                                     int32_t trackCount) {
    auto result = choc::value::createObject("LatencyCompensationState");
    result.setMember("playbackCompensationMode",
                     playbackCompensationModeToString(manager.playbackCompensationMode()));
    result.setMember("inputMonitoringPolicy",
                     inputMonitoringPolicyToString(manager.inputMonitoringPolicy()));
    auto tracks = choc::value::createEmptyArray();
    for (int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        auto track = choc::value::createObject("LatencyCompensationTrackState");
        track.setMember("trackId", script_ids::trackId(trackIndex));
        track.setMember("recordArmed", manager.trackRecordArmed(trackIndex));
        track.setMember("monitoringEnabled", manager.trackMonitoringEnabled(trackIndex));
        tracks.addArrayElement(track);
    }
    result.setMember("tracks", tracks);
    return result;
}

choc::value::Value serializeLatencyCompensationDebugState(uapmd::LatencyCompensationManager& manager) {
    auto result = choc::value::createObject("LatencyCompensationDebugState");
    result.setMember("realtimeInfiniteTailPolicy",
                     realtimeInfiniteTailPolicyToString(manager.realtimeInfiniteTailPolicy()));
    return result;
}

std::string automationTypeToString(uapmd::MidiAutomationEvent::Type type) {
    using T = uapmd::MidiAutomationEvent::Type;
    switch (type) {
        case T::PitchBend: return "pitchBend";
        case T::PerNotePitchBend: return "perNotePitchBend";
        case T::ChannelPressure: return "channelPressure";
        case T::PolyPressure: return "polyPressure";
        case T::ControlChange: return "controlChange";
        case T::RPN: return "rpn";
        case T::NRPN: return "nrpn";
        case T::PerNoteParameter: return "perNoteParameter";
    }
    return "controlChange";
}

// Lane status as the piano roll addresses automation in selection regions.
uint8_t automationLaneStatus(const uapmd::MidiAutomationEvent& point) {
    using T = uapmd::MidiAutomationEvent::Type;
    switch (point.type) {
        case T::PitchBend: return 0xE0;
        case T::PerNotePitchBend: return 0x60;
        case T::ChannelPressure: return 0xD0;
        case T::PolyPressure: return 0xA0;
        case T::ControlChange: return 0xB0;
        case T::RPN: return 0x20;
        case T::NRPN: return 0x30;
        case T::PerNoteParameter: return point.registered_per_note ? 0x00 : 0x10;
    }
    return 0xB0;
}

int64_t clipSecondsToTick(double seconds, const uapmd::MidiRawData& raw) {
    return std::llround(seconds * std::max(1.0, raw.clipTempo) * raw.tickResolution / 60.0);
}

choc::value::Value serializeAutomationPoint(const uapmd::MidiAutomationEvent& point,
                                            const uapmd::ProjectObjectId& clipId,
                                            const uapmd::ProjectObjectId* noteId,
                                            const uapmd::MidiRawData& raw) {
    auto lane = choc::value::createObject("AutomationLane");
    lane.setMember("clipId", clipId);
    if (noteId)
        lane.setMember("noteId", *noteId);
    lane.setMember("group", static_cast<int32_t>(point.umpGroup));
    lane.setMember("channel", static_cast<int32_t>(point.channel));
    lane.setMember("status", static_cast<int32_t>(automationLaneStatus(point)));
    lane.setMember("parameter", static_cast<int32_t>(point.paramIndex));
    auto obj = choc::value::createObject("AutomationPoint");
    obj.setMember("pointId", point.reference_id);
    obj.setMember("type", automationTypeToString(point.type));
    obj.setMember("timeSeconds", point.timeSeconds);
    obj.setMember("tick", choc::value::createInt64(clipSecondsToTick(point.timeSeconds, raw)));
    obj.setMember("value", point.normalizedValue);
    obj.setMember("lane", lane);
    return obj;
}

choc::value::Value serializeClipNotes(const std::string& clipId) {
    auto result = choc::value::createObject("ClipNotes");
    auto content = clipId.empty() ? nullptr : script_ids::document().midiContent(clipId);
    if (!content || !content->rawMidiData) {
        result.setMember("success", false);
        result.setMember("error", "MIDI clip not found");
        return result;
    }
    content->ensureIdentities();
    const auto& raw = *content->rawMidiData;
    auto notes = choc::value::createEmptyArray();
    for (const auto& note : content->editNotes) {
        if (note.deleted)
            continue;
        auto obj = choc::value::createObject("Note");
        obj.setMember("noteId", note.reference_id);
        obj.setMember("startSeconds", note.startSeconds);
        obj.setMember("durationSeconds", note.durationSeconds);
        obj.setMember("startTick", choc::value::createInt64(clipSecondsToTick(note.startSeconds, raw)));
        obj.setMember("endTick", choc::value::createInt64(clipSecondsToTick(note.startSeconds + note.durationSeconds, raw)));
        obj.setMember("note", static_cast<int32_t>(note.note));
        obj.setMember("velocity", note.velocity);
        obj.setMember("channel", static_cast<int32_t>(note.channel));
        obj.setMember("group", static_cast<int32_t>(note.ump_group));
        auto automation = choc::value::createEmptyArray();
        for (const auto& point : note.automationEvents)
            automation.addArrayElement(serializeAutomationPoint(point, clipId, &note.reference_id, raw));
        obj.setMember("automation", automation);
        notes.addArrayElement(obj);
    }
    auto automation = choc::value::createEmptyArray();
    for (const auto& point : content->editClipEvents)
        automation.addArrayElement(serializeAutomationPoint(point, clipId, nullptr, raw));
    result.setMember("success", true);
    result.setMember("clipId", clipId);
    result.setMember("tickResolution", static_cast<int32_t>(raw.tickResolution));
    result.setMember("tempo", raw.clipTempo);
    result.setMember("notes", notes);
    result.setMember("automation", automation);
    return result;
}

std::string objectKindToString(uapmd::ProjectObjectKind kind) {
    switch (kind) {
        case uapmd::ProjectObjectKind::Track: return "track";
        case uapmd::ProjectObjectKind::Clip: return "clip";
        case uapmd::ProjectObjectKind::Note: return "note";
        case uapmd::ProjectObjectKind::AutomationPoint: return "automationPoint";
    }
    return "track";
}

choc::value::Value serializeIdList(const std::vector<uapmd::ProjectObjectId>& ids) {
    auto array = choc::value::createEmptyArray();
    for (const auto& id : ids)
        array.addArrayElement(id);
    return array;
}

template <typename T>
choc::value::Value serializeRange(const std::pair<T, T>& range) {
    auto array = choc::value::createEmptyArray();
    array.addArrayElement(range.first);
    array.addArrayElement(range.second);
    return array;
}

choc::value::Value serializeSelectionRegion(const uapmd::ProjectSelectionRegion& region) {
    auto obj = choc::value::createObject("SelectionRegion");
    if (const auto* timeline = std::get_if<uapmd::TimelineSelectionRegion>(&region)) {
        obj.setMember("type", "timeline");
        obj.setMember("startSeconds", timeline->start_seconds);
        obj.setMember("endSeconds", timeline->end_seconds);
        if (timeline->track_ids)
            obj.setMember("trackIds", serializeIdList(*timeline->track_ids));
    } else if (const auto* notes = std::get_if<uapmd::MidiNoteSelectionRegion>(&region)) {
        obj.setMember("type", "notes");
        obj.setMember("clipId", notes->clip_id);
        obj.setMember("startTick", choc::value::createInt64(static_cast<int64_t>(notes->start_tick)));
        obj.setMember("endTick", choc::value::createInt64(static_cast<int64_t>(notes->end_tick)));
        if (notes->pitches)
            obj.setMember("pitches", serializeRange(std::pair<int32_t, int32_t>{notes->pitches->first, notes->pitches->second}));
        if (notes->group)
            obj.setMember("group", static_cast<int32_t>(*notes->group));
        if (notes->channel)
            obj.setMember("channel", static_cast<int32_t>(*notes->channel));
    } else if (const auto* automation = std::get_if<uapmd::MidiAutomationSelectionRegion>(&region)) {
        auto lane = choc::value::createObject("AutomationLane");
        lane.setMember("clipId", automation->lane.clip_id);
        if (automation->lane.note_id)
            lane.setMember("noteId", *automation->lane.note_id);
        lane.setMember("group", static_cast<int32_t>(automation->lane.group));
        lane.setMember("channel", static_cast<int32_t>(automation->lane.channel));
        lane.setMember("status", static_cast<int32_t>(automation->lane.status));
        lane.setMember("parameter", static_cast<int32_t>(automation->lane.parameter));
        obj.setMember("type", "automation");
        obj.setMember("lane", lane);
        obj.setMember("startTick", choc::value::createInt64(static_cast<int64_t>(automation->start_tick)));
        obj.setMember("endTick", choc::value::createInt64(static_cast<int64_t>(automation->end_tick)));
        if (automation->values)
            obj.setMember("values", serializeRange(*automation->values));
    }
    return obj;
}

choc::value::Value serializeSelection(const uapmd::ProjectSelection& selection) {
    auto regions = choc::value::createEmptyArray();
    for (const auto& region : selection.regions)
        regions.addArrayElement(serializeSelectionRegion(region));
    auto result = choc::value::createObject("Selection");
    result.setMember("objects", serializeIdList(selection.objects));
    result.setMember("regions", regions);
    return result;
}

std::optional<choc::value::ValueView> optionalMember(const choc::value::ValueView& value, const char* key) {
    if (!value.isObject() || !value.hasObjectMember(key) || value[key].isVoid())
        return std::nullopt;
    return value[key];
}

bool parseNumber(const choc::value::ValueView& value, const char* key, double& out, std::string& error) {
    auto member = optionalMember(value, key);
    if (!member || !(member->isInt() || member->isFloat())) {
        error = std::string(key) + " must be a number";
        return false;
    }
    out = member->get<double>();
    return true;
}

bool parseInteger(const choc::value::ValueView& value, const char* key, int64_t minimum, int64_t maximum,
                  int64_t& out, std::string& error) {
    double number = 0;
    if (!parseNumber(value, key, number, error))
        return false;
    if (number != std::floor(number) || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        error = std::string(key) + " must be an integer in [" + std::to_string(minimum) + ", " + std::to_string(maximum) + "]";
        return false;
    }
    out = static_cast<int64_t>(number);
    return true;
}

template <typename T>
bool parseOptionalInteger(const choc::value::ValueView& value, const char* key, int64_t maximum,
                          std::optional<T>& out, std::string& error) {
    if (!optionalMember(value, key))
        return true;
    int64_t number = 0;
    if (!parseInteger(value, key, 0, maximum, number, error))
        return false;
    out = static_cast<T>(number);
    return true;
}

bool parseString(const choc::value::ValueView& value, const char* key, std::string& out, std::string& error) {
    auto member = optionalMember(value, key);
    if (!member || !member->isString() || member->getString().empty()) {
        error = std::string(key) + " must be a non-empty string";
        return false;
    }
    out = std::string(member->getString());
    return true;
}

bool parseOptionalRange(const choc::value::ValueView& value, const char* key,
                        std::optional<std::pair<double, double>>& out, std::string& error) {
    auto member = optionalMember(value, key);
    if (!member)
        return true;
    if (!member->isArray() || member->size() != 2 ||
        !((*member)[0].isInt() || (*member)[0].isFloat()) || !((*member)[1].isInt() || (*member)[1].isFloat())) {
        error = std::string(key) + " must be a [low, high] number pair";
        return false;
    }
    out = std::pair{(*member)[0].get<double>(), (*member)[1].get<double>()};
    return true;
}

constexpr int64_t kMaxScriptTick = int64_t{1} << 53;

bool parseSelectionRegion(const choc::value::ValueView& value, uapmd::ProjectSelectionRegion& region, std::string& error) {
    std::string type;
    if (!value.isObject() || !parseString(value, "type", type, error)) {
        error = "region must be an object with a type";
        return false;
    }
    if (type == "timeline") {
        uapmd::TimelineSelectionRegion timeline;
        if (!parseNumber(value, "startSeconds", timeline.start_seconds, error) ||
            !parseNumber(value, "endSeconds", timeline.end_seconds, error))
            return false;
        if (auto ids = optionalMember(value, "trackIds")) {
            if (!ids->isArray()) {
                error = "trackIds must be an array";
                return false;
            }
            timeline.track_ids.emplace();
            for (const auto& id : *ids) {
                if (!id.isString()) {
                    error = "trackIds must contain strings";
                    return false;
                }
                timeline.track_ids->emplace_back(id.getString());
            }
        }
        region = std::move(timeline);
        return true;
    }
    int64_t startTick = 0, endTick = 0;
    if (type == "notes") {
        uapmd::MidiNoteSelectionRegion notes;
        std::optional<std::pair<double, double>> pitches;
        if (!parseString(value, "clipId", notes.clip_id, error) ||
            !parseInteger(value, "startTick", 0, kMaxScriptTick, startTick, error) ||
            !parseInteger(value, "endTick", 0, kMaxScriptTick, endTick, error) ||
            !parseOptionalRange(value, "pitches", pitches, error) ||
            !parseOptionalInteger(value, "group", 15, notes.group, error) ||
            !parseOptionalInteger(value, "channel", 15, notes.channel, error))
            return false;
        if (pitches) {
            const auto [low, high] = *pitches;
            if (low != std::floor(low) || high != std::floor(high) || low < 0 || high > 127) {
                error = "pitches must be integers in [0, 127]";
                return false;
            }
            notes.pitches = std::pair{static_cast<uint8_t>(low), static_cast<uint8_t>(high)};
        }
        notes.start_tick = static_cast<uint64_t>(startTick);
        notes.end_tick = static_cast<uint64_t>(endTick);
        region = std::move(notes);
        return true;
    }
    if (type == "automation") {
        uapmd::MidiAutomationSelectionRegion automation;
        auto lane = optionalMember(value, "lane");
        if (!lane || !lane->isObject()) {
            error = "lane must be an object";
            return false;
        }
        std::optional<uint8_t> group, channel, status;
        std::optional<uint16_t> parameter;
        if (!parseString(*lane, "clipId", automation.lane.clip_id, error) ||
            !parseOptionalInteger(*lane, "group", 15, group, error) ||
            !parseOptionalInteger(*lane, "channel", 15, channel, error) ||
            !parseOptionalInteger(*lane, "status", 0xFF, status, error) ||
            !parseOptionalInteger(*lane, "parameter", 0xFFFF, parameter, error) ||
            !parseInteger(value, "startTick", 0, kMaxScriptTick, startTick, error) ||
            !parseInteger(value, "endTick", 0, kMaxScriptTick, endTick, error) ||
            !parseOptionalRange(value, "values", automation.values, error))
            return false;
        if (optionalMember(*lane, "noteId")) {
            automation.lane.note_id.emplace();
            if (!parseString(*lane, "noteId", *automation.lane.note_id, error))
                return false;
        }
        automation.lane.group = group.value_or(automation.lane.group);
        automation.lane.channel = channel.value_or(automation.lane.channel);
        automation.lane.status = status.value_or(automation.lane.status);
        automation.lane.parameter = parameter.value_or(automation.lane.parameter);
        automation.start_tick = static_cast<uint64_t>(startTick);
        automation.end_tick = static_cast<uint64_t>(endTick);
        region = std::move(automation);
        return true;
    }
    error = "region type must be timeline, notes, or automation";
    return false;
}

bool parseSelection(const choc::value::ValueView& value, uapmd::ProjectSelection& selection, std::string& error) {
    if (!value.isObject()) {
        error = "selection must be an object";
        return false;
    }
    if (auto objects = optionalMember(value, "objects")) {
        if (!objects->isArray()) {
            error = "objects must be an array";
            return false;
        }
        for (const auto& id : *objects) {
            if (!id.isString()) {
                error = "objects must contain strings";
                return false;
            }
            selection.objects.emplace_back(id.getString());
        }
    }
    if (auto regions = optionalMember(value, "regions")) {
        if (!regions->isArray()) {
            error = "regions must be an array";
            return false;
        }
        for (const auto& item : *regions) {
            uapmd::ProjectSelectionRegion region;
            if (!parseSelectionRegion(item, region, error))
                return false;
            selection.regions.push_back(std::move(region));
        }
    }
    return true;
}

// A negative context selects the active context, falling back to the default one.
uapmd::SelectionContextId resolveSelectionContext(int64_t context) {
    const auto& selections = script_ids::document().selections();
    if (context >= 0)
        return static_cast<uapmd::SelectionContextId>(context);
    return selections.activeContext().value_or(selections.defaultContext());
}

} // namespace

void UapmdJSRuntime::registerTimelineAPI()
{
    impl_->context.registerFunction ("__remidy_timeline_get_state", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        const auto& state = uapmd_app::AppModel::instance().timeline();
        auto obj = choc::value::createObject ("TimelineState");
        obj.setMember ("tempo", state.tempo);
        obj.setMember ("timeSignatureNumerator", state.timeSignatureNumerator);
        obj.setMember ("timeSignatureDenominator", state.timeSignatureDenominator);
        obj.setMember ("isPlaying", state.isPlaying);
        obj.setMember ("playheadSamples", choc::value::createInt64 (state.playheadPosition.samples));
        obj.setMember ("loopEnabled", state.loopEnabled);
        obj.setMember ("loopStartSamples", choc::value::createInt64 (state.loopStart.samples));
        obj.setMember ("loopEndSamples", choc::value::createInt64 (state.loopEnd.samples));
        return obj;
    });

    impl_->context.registerFunction ("__remidy_timeline_get_latency_compensation_state", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& appModel = uapmd_app::AppModel::instance();
        auto* engine = appModel.sequencer().engine();
        if (!engine || !engine->latencyCompensationManager())
            return choc::value::createObject("LatencyCompensationState");
        const auto trackCount = static_cast<int32_t>(engine->tracks().size());
        return serializeLatencyCompensationState(*engine->latencyCompensationManager(), trackCount);
    });

    impl_->context.registerFunction ("__remidy_timeline_set_latency_compensation_state", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto result = choc::value::createObject("LatencyCompensationMutation");
        if (args.size() < 1 || args[0] == nullptr || !args[0]->isObject()) {
            result.setMember("success", false);
            result.setMember("error", "payload must be an object");
            return result;
        }

        auto& appModel = uapmd_app::AppModel::instance();
        auto* engine = appModel.sequencer().engine();
        auto* manager = engine ? engine->latencyCompensationManager() : nullptr;
        if (!manager) {
            result.setMember("success", false);
            result.setMember("error", "latency compensation manager is unavailable");
            return result;
        }

        auto settings = manager->projectSettings();
        bool hasMutation = false;
        auto payload = args[0]->getView();
        if (payload.hasObjectMember("playbackCompensationMode")) {
            uapmd::PlaybackCompensationMode mode;
            const auto text = payload["playbackCompensationMode"].get<std::string>();
            if (!parsePlaybackCompensationMode(text, mode)) {
                result.setMember("success", false);
                result.setMember("error", "playbackCompensationMode is invalid");
                return result;
            }
            settings.playback_compensation_mode = mode;
            hasMutation = true;
        }
        if (payload.hasObjectMember("inputMonitoringPolicy")) {
            uapmd::InputMonitoringPolicy policy;
            const auto text = payload["inputMonitoringPolicy"].get<std::string>();
            if (!parseInputMonitoringPolicy(text, policy)) {
                result.setMember("success", false);
                result.setMember("error", "inputMonitoringPolicy is invalid");
                return result;
            }
            settings.input_monitoring_policy = policy;
            hasMutation = true;
        }
        if (payload.hasObjectMember("tracks")) {
            auto tracks = payload["tracks"];
            if (!tracks.isArray()) {
                result.setMember("success", false);
                result.setMember("error", "tracks must be an array");
                return result;
            }
            for (uint32_t i = 0; i < tracks.size(); ++i) {
                const auto& trackValue = tracks[i];
                if (!trackValue.isObject() || !trackValue.hasObjectMember("trackId")) {
                    result.setMember("success", false);
                    result.setMember("error", "track entry must contain trackId");
                    return result;
                }
                const auto resolved = script_ids::trackIndex(trackValue["trackId"].getWithDefault<std::string>(""));
                if (!resolved || *resolved < 0) {
                    result.setMember("success", false);
                    result.setMember("error", "trackId is invalid");
                    return result;
                }
                const auto trackIndex = *resolved;
                if (trackValue.hasObjectMember("recordArmed")) {
                    std::erase(settings.record_armed_track_indexes, trackIndex);
                    if (trackValue["recordArmed"].get<bool>())
                        settings.record_armed_track_indexes.push_back(trackIndex);
                }
                if (trackValue.hasObjectMember("monitoringEnabled")) {
                    std::erase(settings.monitored_track_indexes, trackIndex);
                    if (trackValue["monitoringEnabled"].get<bool>())
                        settings.monitored_track_indexes.push_back(trackIndex);
                }
            }
            hasMutation = true;
        }

        if (hasMutation
            && !engine->commands().setLatencyCompensationSettings(settings)) {
            result.setMember("success", false);
            result.setMember("error", "failed to update latency compensation settings");
            return result;
        }
        result.setMember("success", true);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_get_latency_compensation_debug_state", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& appModel = uapmd_app::AppModel::instance();
        auto* engine = appModel.sequencer().engine();
        if (!engine || !engine->latencyCompensationManager())
            return choc::value::createObject("LatencyCompensationDebugState");
        return serializeLatencyCompensationDebugState(*engine->latencyCompensationManager());
    });

    impl_->context.registerFunction ("__remidy_timeline_set_latency_compensation_debug_state", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto result = choc::value::createObject("LatencyCompensationDebugMutation");
        if (args.size() < 1 || args[0] == nullptr || !args[0]->isObject()) {
            result.setMember("success", false);
            result.setMember("error", "payload must be an object");
            return result;
        }

        auto& appModel = uapmd_app::AppModel::instance();
        auto* engine = appModel.sequencer().engine();
        auto* manager = engine ? engine->latencyCompensationManager() : nullptr;
        if (!manager) {
            result.setMember("success", false);
            result.setMember("error", "latency compensation manager is unavailable");
            return result;
        }

        auto payload = args[0]->getView();
        if (payload.hasObjectMember("realtimeInfiniteTailPolicy")) {
            uapmd::RealtimeInfiniteTailPolicy policy;
            const auto text = payload["realtimeInfiniteTailPolicy"].get<std::string>();
            if (!parseRealtimeInfiniteTailPolicy(text, policy)) {
                result.setMember("success", false);
                result.setMember("error", "realtimeInfiniteTailPolicy is invalid");
                return result;
            }
            manager->realtimeInfiniteTailPolicy(policy);
        }

        result.setMember("success", true);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_set_tempo", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto bpm = args.get<double> (0, 120.0);
        if (bpm > 0.0)
            uapmd_app::AppModel::instance().timeline().tempo = bpm;
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_timeline_get_master_track_id", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        return choc::value::createString (script_ids::masterTrackId());
    });

    impl_->context.registerFunction ("__remidy_timeline_get_clips", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto trackId = args.get<std::string> (0, "");
        auto arr = choc::value::createEmptyArray();
        auto* track = script_ids::document().addresses().timelineTrack(trackId);
        if (trackId.empty() || !track)
            return arr;

        for (const auto& snapshot : script_ids::clipsOnTrack(trackId))
        {
            const auto* clip = track->clipManager().getClip(snapshot.clipNumericId);
            if (!clip)
                continue;
            auto obj = choc::value::createObject ("ClipData");
            obj.setMember ("clipId", snapshot.clipId);
            obj.setMember ("positionSamples", choc::value::createInt64 (clip->position.samples));
            obj.setMember ("durationSamples", choc::value::createInt64 (clip->durationSamples));
            obj.setMember ("sourceOffsetSamples", choc::value::createInt64 (clip->sourceOffsetSamples));
            obj.setMember ("name", clip->name);
            obj.setMember ("filepath", clip->filepath);
            obj.setMember ("clipType", clip->clipType == uapmd::ClipType::Midi ? std::string ("midi") : std::string ("audio"));
            obj.setMember ("gain", clip->gain);
            obj.setMember ("muted", clip->muted);
            obj.setMember ("tempo", clip->clipTempo);
            obj.setMember ("tickResolution", static_cast<int32_t> (clip->tickResolution));
            obj.setMember ("anchorReferenceId", clip->anchorReferenceId);
            arr.addArrayElement (obj);
        }
        return arr;
    });

    impl_->context.registerFunction ("__remidy_timeline_ensure_dag_graph", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = script_ids::trackIndex(args.get<std::string> (0, ""));
        return choc::value::createBool(trackIndex && uapmd_app::AppModel::instance().ensureTrackUsesEditorGraph(*trackIndex));
    });

    impl_->context.registerFunction ("__remidy_timeline_show_track_graph", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        if (auto trackIndex = script_ids::trackIndex(args.get<std::string> (0, "")))
            uapmd_app::AppModel::instance().requestShowTrackGraph(*trackIndex);
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_timeline_revert_track_graph_to_simple", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = script_ids::trackIndex(args.get<std::string> (0, ""));
        return choc::value::createBool(trackIndex && uapmd_app::AppModel::instance().revertTrackToSimpleGraph(*trackIndex));
    });

    impl_->context.registerFunction ("__remidy_timeline_get_track_graph_connections", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = script_ids::trackIndex(args.get<std::string> (0, "")).value_or(-1);
        std::vector<AudioPluginGraphConnection> connections;
        std::string error;

        auto result = choc::value::createObject ("TrackGraphConnections");
        if (!uapmd_app::AppModel::instance().getTrackGraphConnections(trackIndex, connections, error)) {
            result.setMember("success", false);
            result.setMember("error", error);
            result.setMember("connections", choc::value::createEmptyArray());
            return result;
        }

        auto array = choc::value::createEmptyArray();
        for (const auto& connection : connections)
            array.addArrayElement(serializeGraphConnection(connection));

        result.setMember("success", true);
        result.setMember("error", "");
        result.setMember("connections", array);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_connect_track_graph", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = script_ids::trackIndex(args.get<std::string>(0, ""));
        auto busTypeText = args.get<std::string>(1, "");
        auto sourceTypeText = args.get<std::string>(2, "");
        auto sourceInstanceId = args.get<int32_t>(3, -1);
        auto sourceBusIndex = args.get<int32_t>(4, 0);
        auto targetTypeText = args.get<std::string>(5, "");
        auto targetInstanceId = args.get<int32_t>(6, -1);
        auto targetBusIndex = args.get<int32_t>(7, 0);
        auto sourceNodeId = args.get<std::string>(8, "");
        auto targetNodeId = args.get<std::string>(9, "");

        auto result = choc::value::createObject("TrackGraphMutation");
        if (!trackIndex) {
            result.setMember("success", false);
            result.setMember("error", "Track not found");
            return result;
        }
        AudioPluginGraphBusType busType;
        AudioPluginGraphEndpointType sourceType, targetType;
        if (!parseGraphBusType(busTypeText, busType) ||
            !parseGraphEndpointType(sourceTypeText, sourceType) ||
            !parseGraphEndpointType(targetTypeText, targetType)) {
            result.setMember("success", false);
            result.setMember("error", "Invalid graph bus or endpoint type");
            return result;
        }

        std::string error;
        const bool ok = uapmd_app::AppModel::instance().connectTrackGraph(
            *trackIndex,
            AudioPluginGraphConnection{
                .id = 0,
                .bus_type = busType,
                .source = makeGraphEndpoint(sourceType, sourceNodeId, sourceInstanceId, sourceBusIndex),
                .target = makeGraphEndpoint(targetType, targetNodeId, targetInstanceId, targetBusIndex),
            },
            error);
        result.setMember("success", ok);
        result.setMember("error", error);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_disconnect_track_graph_connection", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = script_ids::trackIndex(args.get<std::string>(0, ""));
        auto connectionId = args.get<int64_t>(1, 0);
        std::string error = trackIndex ? std::string{} : std::string{"Track not found"};
        const bool ok = trackIndex && uapmd_app::AppModel::instance().disconnectTrackGraphConnection(*trackIndex, connectionId, error);
        auto result = choc::value::createObject("TrackGraphMutation");
        result.setMember("success", ok);
        result.setMember("error", error);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_get_clip_audio_events", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip = script_ids::clip(args.get<std::string> (0, ""));
        std::vector<uapmd::ClipMarker> markers;
        std::vector<uapmd::AudioWarpPoint> warps;
        std::string error = "Clip not found";
        auto result = choc::value::createObject ("ClipAudioEvents");
        if (!clip || !uapmd_app::AppModel::instance().getClipAudioEvents(clip->trackIndex, clip->clipNumericId, markers, warps, error)) {
            result.setMember ("success", false);
            result.setMember ("error", error);
            return result;
        }

        const auto ownerReferenceId = script_ids::markerOwnerReferenceId(*clip, kMasterMarkerReferenceId);
        auto markerArray = choc::value::createEmptyArray();
        for (const auto& marker : markers)
            markerArray.addArrayElement(serializeMarker(marker, ownerReferenceId));
        auto warpArray = choc::value::createEmptyArray();
        for (const auto& warp : warps)
            warpArray.addArrayElement(serializeWarp(warp, ownerReferenceId));

        result.setMember ("success", true);
        result.setMember ("markers", markerArray);
        result.setMember ("audioWarps", warpArray);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_set_clip_audio_events", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip = script_ids::clip(args.get<std::string> (0, ""));
        auto result = choc::value::createObject ("ClipAudioEventSetResult");
        if (!clip || args.size() < 2 || args[1] == nullptr || !args[1]->isObject()) {
            result.setMember ("success", false);
            result.setMember ("error", clip ? "invalid arguments" : "Clip not found");
            return result;
        }

        const auto ownerReferenceId = script_ids::markerOwnerReferenceId(*clip, kMasterMarkerReferenceId);
        std::vector<uapmd::ClipMarker> markers;
        std::vector<uapmd::AudioWarpPoint> warps;
        std::string error;
        auto payload = args[1]->getView();
        auto markersView = payload.hasObjectMember("markers") ? std::optional(payload["markers"]) : std::nullopt;
        auto warpsView = payload.hasObjectMember("audioWarps") ? std::optional(payload["audioWarps"]) : std::nullopt;
        if (!parseMarkersValue(markersView ? &*markersView : nullptr, ownerReferenceId, markers, error) ||
            !parseWarpsValue(warpsView ? &*warpsView : nullptr, ownerReferenceId, warps, error) ||
            !uapmd_app::AppModel::instance().setClipAudioEvents(clip->trackIndex, clip->clipNumericId, std::move(markers), std::move(warps), error)) {
            result.setMember ("success", false);
            result.setMember ("error", error);
            return result;
        }

        result.setMember ("success", true);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_get_master_markers", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto array = choc::value::createEmptyArray();
        for (const auto& marker : uapmd_app::AppModel::instance().sequencer().engine()->masterTrackMarkers())
            array.addArrayElement(serializeMarker(marker, kMasterMarkerReferenceId));
        return array;
    });

    impl_->context.registerFunction ("__remidy_timeline_set_master_markers", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto result = choc::value::createObject ("MasterMarkerSetResult");
        std::vector<uapmd::ClipMarker> markers;
        std::string error;
        auto markersView = (args.size() > 0 && args[0] != nullptr) ? std::optional(args[0]->getView()) : std::nullopt;
        if (!parseMarkersValue(markersView ? &*markersView : nullptr, kMasterMarkerReferenceId, markers, error) ||
            !uapmd_app::AppModel::instance().setMasterTrackMarkersWithValidation(std::move(markers), error)) {
            result.setMember ("success", false);
            result.setMember ("error", error);
            return result;
        }
        result.setMember ("success", true);
        return result;
    });

    impl_->context.registerFunction ("__remidy_timeline_add_midi_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = script_ids::trackIndex(args.get<std::string> (0, ""));
        auto positionSamples = args.get<int64_t> (1, 0);
        auto filepath = args.get<std::string> (2, "");

        auto makeError = [] (const std::string& msg) {
            auto obj = choc::value::createObject ("ClipAddResult");
            obj.setMember ("clipId", std::string{});
            obj.setMember ("success", false);
            obj.setMember ("error", msg);
            return obj;
        };

        if (!trackIndex || *trackIndex < 0 || filepath.empty())
            return makeError ("invalid arguments");

        auto& appModel = uapmd_app::AppModel::instance();
        uapmd::TimelinePosition pos{};
        pos.samples = positionSamples;
        auto result = appModel.addMidiClipToTrack (*trackIndex, pos, filepath);

        auto obj = choc::value::createObject ("ClipAddResult");
        obj.setMember ("clipId", result.success ? script_ids::clipId(*trackIndex, result.clipId) : std::string{});
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        return obj;
    });

    impl_->context.registerFunction ("__remidy_timeline_import_midi_tracks", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto filepath = args.get<std::string> (0, "");
        auto job = createMutationJob();
        if (filepath.empty()) {
            completeMutationJob(
                job,
                choc::value::createObject("MidiTracksImportResult"),
                "Invalid file path");
            return mutationJobValue(job);
        }

        uapmd_app::AppModel::instance().importMidiTracksFromFile(
            filepath,
            [this, job](uapmd_app::AppModel::MidiTracksImportResult result) {
                auto value = choc::value::createObject("MidiTracksImportResult");
                value.setMember("success", result.success);
                value.setMember("error", result.error);

                auto warnings = choc::value::createEmptyArray();
                for (const auto& warning : result.warnings)
                    warnings.addArrayElement(warning);
                value.setMember("warnings", warnings);

                auto importedTracks = choc::value::createEmptyArray();
                for (const auto& track : result.importedTracks) {
                    auto trackValue = choc::value::createObject("MidiTrackImportResult");
                    trackValue.setMember("trackId", track.trackIndex < 0 ? std::string{} : script_ids::trackId(track.trackIndex));
                    trackValue.setMember("clipName", track.clipName);
                    trackValue.setMember("success", track.success);
                    trackValue.setMember("error", track.error);
                    importedTracks.addArrayElement(trackValue);
                }
                value.setMember("importedTracks", importedTracks);
                completeMutationJob(
                    job,
                    std::move(value),
                    result.success ? std::string{} : std::move(result.error));
            });
        return mutationJobValue(job);
    });

    impl_->context.registerFunction ("__remidy_timeline_remove_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip = script_ids::clip(args.get<std::string> (0, ""));
        if (!clip)
            return choc::value::createBool (false);
        return choc::value::createBool (uapmd_app::AppModel::instance().removeClipFromTrack (clip->trackIndex, clip->clipNumericId));
    });

    impl_->context.registerFunction ("__remidy_timeline_get_clip_ump_events", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip = script_ids::clip(args.get<std::string> (0, ""));
        if (!clip)
            return choc::value::createObject ("");
        return toChocValue(uapmd_app::AppModel::instance().getMidiClipUmpEvents(clip->trackIndex, clip->clipNumericId));
    });

    impl_->context.registerFunction ("__remidy_timeline_get_clip_notes", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        return serializeClipNotes(args.get<std::string> (0, ""));
    });

    impl_->context.registerFunction ("__remidy_timeline_add_ump_event", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip = script_ids::clip(args.get<std::string> (0, ""));
        auto tick       = static_cast<uint64_t>(args.get<int64_t> (1, 0));
        if (!clip)
            return choc::value::createBool (false);
        // args[2] is an array of 1–2 UMP words
        std::vector<uint32_t> words;
        if (args.size() > 2) {
            auto* wordsVal = args[2];
            if (wordsVal && wordsVal->isArray())
                for (uint32_t i = 0; i < wordsVal->size(); ++i)
                    words.push_back (static_cast<uint32_t>((*wordsVal)[i].get<int64_t>()));
        }
        if (words.empty()) return choc::value::createBool (false);
        std::string error;
        return choc::value::createBool (
            uapmd_app::AppModel::instance().addUmpEventToClip (clip->trackIndex, clip->clipNumericId, tick, std::move(words), error));
    });

    impl_->context.registerFunction ("__remidy_timeline_remove_ump_event", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip  = script_ids::clip(args.get<std::string> (0, ""));
        auto eventIndex  = args.get<int32_t> (1, -1);
        if (!clip || eventIndex < 0)
            return choc::value::createBool (false);
        std::string error;
        return choc::value::createBool (
            uapmd_app::AppModel::instance().removeUmpEventFromClip (clip->trackIndex, clip->clipNumericId, eventIndex, error));
    });

    impl_->context.registerFunction ("__remidy_timeline_create_empty_midi_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex      = script_ids::trackIndex(args.get<std::string> (0, ""));
        if (!trackIndex || *trackIndex < 0) return choc::value::createString ("");
        auto positionSamples = args.get<int64_t>  (1, 0);
        auto tickResolution  = static_cast<uint32_t>(std::max (1, args.get<int32_t> (2, 480)));
        auto bpm             = args.get<double>   (3, 120.0);
        auto r = uapmd_app::AppModel::instance().createEmptyMidiClip (
            *trackIndex, positionSamples, tickResolution, bpm);
        return choc::value::createString (r.success ? script_ids::clipId(*trackIndex, r.clipId) : std::string{});
    });
}

void UapmdJSRuntime::registerSelectionAPI()
{
    impl_->context.registerFunction ("__remidy_selection_get_contexts", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        const auto& selections = script_ids::document().selections();
        const auto active = selections.activeContext();
        auto result = choc::value::createObject ("SelectionContexts");
        result.setMember ("defaultContext", choc::value::createInt64 (static_cast<int64_t> (selections.defaultContext())));
        result.setMember ("activeContext", active ? choc::value::createInt64 (static_cast<int64_t> (*active)) : choc::value::Value());
        return result;
    });

    impl_->context.registerFunction ("__remidy_selection_context_for_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto clip = script_ids::clip (args.get<std::string> (0, ""));
        auto* session = clip ? uapmd_app::AppModel::instance().findPianoRollSession (clip->trackIndex, clip->clipNumericId) : nullptr;
        if (!session || !script_ids::document().selections().hasContext (session->selection_context))
            return choc::value::Value();
        return choc::value::createInt64 (static_cast<int64_t> (session->selection_context));
    });

    impl_->context.registerFunction ("__remidy_selection_get", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto context = resolveSelectionContext (args.get<int64_t> (0, -1));
        const auto selection = script_ids::document().selections().snapshot (context);
        return selection ? serializeSelection (*selection) : choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_selection_set", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto result = choc::value::createObject ("SelectionUpdateResult");
        uapmd::ProjectSelection selection;
        std::string error;
        if (args.size() < 1 || args[0] == nullptr || !parseSelection (args[0]->getView(), selection, error)) {
            result.setMember ("success", false);
            result.setMember ("error", error.empty() ? std::string ("selection is required") : error);
            return result;
        }
        const auto context = resolveSelectionContext (args.get<int64_t> (1, -1));
        const auto update = script_ids::document().selections().replace (context, std::move (selection));
        result.setMember ("success", update.success);
        result.setMember ("error", update.error);
        return result;
    });

    impl_->context.registerFunction ("__remidy_selection_clear", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto& selections = script_ids::document().selections();
        const auto context = resolveSelectionContext (args.get<int64_t> (0, -1));
        if (!selections.hasContext (context))
            return choc::value::createBool (false);
        selections.clear (context);
        return choc::value::createBool (true);
    });

    // A negative context leaves no context active.
    impl_->context.registerFunction ("__remidy_selection_set_active_context", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto context = args.get<int64_t> (0, -1);
        return choc::value::createBool (script_ids::document().selections().setActiveContext (
            context < 0 ? std::nullopt : std::optional<uapmd::SelectionContextId> (static_cast<uapmd::SelectionContextId> (context))));
    });

    impl_->context.registerFunction ("__remidy_selection_get_object", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        const auto id = args.get<std::string> (0, "");
        const auto object = id.empty() ? std::nullopt : script_ids::document().view().getObject (id);
        if (!object)
            return choc::value::Value();
        auto result = choc::value::createObject ("DocumentObject");
        result.setMember ("id", object->id);
        result.setMember ("kind", objectKindToString (object->kind));
        if (object->parent_id)
            result.setMember ("parentId", *object->parent_id);
        return result;
    });

    impl_->context.registerFunction ("__remidy_selection_get_projection", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto& document = script_ids::document();
        const auto selection = document.selections().snapshot (resolveSelectionContext (args.get<int64_t> (0, -1)));
        if (!selection)
            return choc::value::Value();
        const auto projection = uapmd::projectSelection (document.view(), *selection);
        auto result = choc::value::createObject ("SelectionProjection");
        result.setMember ("clipIds", serializeIdList (projection.clip_ids));
        result.setMember ("trackIds", serializeIdList (projection.track_ids));
        result.setMember ("timeRange", projection.time_range ? serializeRange (*projection.time_range) : choc::value::Value());
        return result;
    });
}

void UapmdJSRuntime::registerRenderAPI()
{
    impl_->context.registerFunction ("__remidy_render_start", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto outputPath = args.get<std::string> (0, "");
        if (outputPath.empty())
            return choc::value::createBool (false);

        uapmd_app::AppModel::RenderToFileSettings settings;
        settings.outputPath = outputPath;
        settings.startSeconds = std::max (0.0, args.get<double> (1, 0.0));

        auto endSeconds = args.get<double> (2, -1.0);
        if (endSeconds > settings.startSeconds)
            settings.endSeconds = endSeconds;

        settings.tailSeconds = std::max (0.0, args.get<double> (3, 2.0));
        settings.useContentFallback = args.get<bool> (4, true);

        auto bounds = uapmd_app::AppModel::instance().timelineContentBounds();
        settings.contentBoundsValid = bounds.hasContent;
        settings.contentStartSeconds = bounds.startSeconds;
        settings.contentEndSeconds = bounds.endSeconds;

        return choc::value::createBool (uapmd_app::AppModel::instance().startRenderToFile (settings));
    });

    impl_->context.registerFunction ("__remidy_render_get_status", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto status = uapmd_app::AppModel::instance().getRenderToFileStatus();
        auto obj = choc::value::createObject ("RenderToFileStatus");
        obj.setMember ("running", status.running);
        obj.setMember ("completed", status.completed);
        obj.setMember ("success", status.success);
        obj.setMember ("progress", status.progress);
        obj.setMember ("renderedSeconds", status.renderedSeconds);
        obj.setMember ("message", status.message);
        obj.setMember ("outputPath", status.outputPath.string());
        return obj;
    });

    impl_->context.registerFunction ("__remidy_render_clear_status", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd_app::AppModel::instance().clearCompletedRenderStatus();
        return choc::value::Value();
    });

    impl_->context.registerFunction ("__remidy_render_cancel", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd_app::AppModel::instance().cancelRenderToFile();
        return choc::value::Value();
    });
}

} // namespace uapmd_app
