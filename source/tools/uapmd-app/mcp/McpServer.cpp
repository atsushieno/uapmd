#include "McpServer.hpp"
#include "../AppModel.hpp"
#include "../UapmdJSRuntime.hpp"

#include <httplib.h>
#include <choc/javascript/choc_javascript.h>
#include <choc/text/choc_JSON.h>
#include <AppJsLib.h>
#include <ResEmbed/ResEmbed.h>

#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>

namespace uapmd {

// ─────────────────────────────────────────────────
//  JSON-RPC 2.0 helpers
// ─────────────────────────────────────────────────

static std::string jsonRpcResult(const choc::value::Value& id, const choc::value::Value& result)
{
    auto resp = choc::value::createObject ("");
    resp.setMember ("jsonrpc", std::string ("2.0"));
    resp.setMember ("id", id);
    resp.setMember ("result", result);
    return choc::json::toString (resp);
}

static std::string jsonRpcError(const choc::value::Value& id, int code, const std::string& message)
{
    auto resp = choc::value::createObject ("");
    resp.setMember ("jsonrpc", std::string ("2.0"));
    resp.setMember ("id", id);
    auto err = choc::value::createObject ("");
    err.setMember ("code", code);
    err.setMember ("message", message);
    resp.setMember ("error", err);
    return choc::json::toString (resp);
}

static choc::value::Value textContent(const std::string& text)
{
    auto arr = choc::value::createEmptyArray();
    auto item = choc::value::createObject ("");
    item.setMember ("type", std::string ("text"));
    item.setMember ("text", text);
    arr.addArrayElement (item);
    return arr;
}

static std::string getStringArg(const choc::value::Value& args, const char* key, const std::string& fallback = "")
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<std::string>();
    return fallback;
}

static int32_t getIntArg(const choc::value::Value& args, const char* key, int32_t fallback = -1)
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<int32_t>();
    return fallback;
}

static int64_t getInt64Arg(const choc::value::Value& args, const char* key, int64_t fallback = 0)
{
    if (args.isObject() && args.hasObjectMember (key))
        return args[key].get<int64_t>();
    return fallback;
}

// ─────────────────────────────────────────────────
//  Tool definitions
// ─────────────────────────────────────────────────

// Use the R"j(...)j" delimiter to avoid premature termination on )"  inside JSON strings.
static choc::value::Value buildToolDefinitions()
{
    auto tools = choc::value::createEmptyArray();

    struct ToolDef { const char* name; const char* description; const char* schemaJson; };

    static const ToolDef defs[] = {
        {
            "list_plugins",
            "List available plugins. Optionally filter by format (VST3, AU, LV2, CLAP).",
            R"j({"type":"object","properties":{"format":{"type":"string","description":"Format filter: VST3, AU, LV2, CLAP"}}})j"
        },
        {
            "list_tracks",
            "List all sequencer tracks with their plugin instances.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "create_track",
            "Create a new empty sequencer track. Returns the new trackIndex.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "add_plugin_to_track",
            "Add a plugin instance to a track. Returns the instanceId.",
            R"j({"type":"object","required":["trackIndex","pluginId","format"],"properties":{"trackIndex":{"type":"integer"},"pluginId":{"type":"string","description":"Plugin ID from list_plugins"},"format":{"type":"string","description":"VST3, AU, LV2, or CLAP"}}})j"
        },
        {
            "get_timeline_state",
            "Get tempo, time signature, playback position, and loop settings.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "set_tempo",
            "Set the timeline tempo in BPM.",
            R"j({"type":"object","required":["bpm"],"properties":{"bpm":{"type":"number"}}})j"
        },
        {
            "list_clips",
            "List all clips on a given track.",
            R"j({"type":"object","required":["trackIndex"],"properties":{"trackIndex":{"type":"integer"}}})j"
        },
        {
            "add_midi_clip",
            "Import a Standard MIDI File (.mid) as a clip on a track.",
            R"j({"type":"object","required":["trackIndex","filepath"],"properties":{"trackIndex":{"type":"integer"},"positionSamples":{"type":"integer","description":"Start in samples (default 0)"},"filepath":{"type":"string","description":"Absolute path to .mid file"}}})j"
        },
        {
            "remove_clip",
            "Remove a clip from a track by clipId.",
            R"j({"type":"object","required":["trackIndex","clipId"],"properties":{"trackIndex":{"type":"integer"},"clipId":{"type":"integer"}}})j"
        },
        {
            "play",
            "Start timeline playback.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "stop",
            "Stop timeline playback.",
            R"j({"type":"object","properties":{}})j"
        },
        {
            "run_script",
            "Execute JavaScript using the uapmd scripting API. "
            "All __remidy_* native functions and the uapmd.* global object are available. "
            "Returns the string representation of the last evaluated expression.",
            R"j({"type":"object","required":["code"],"properties":{"code":{"type":"string","description":"JavaScript code to execute"}}})j"
        },
    };

    for (const auto& def : defs)
    {
        auto t = choc::value::createObject ("");
        t.setMember ("name", std::string (def.name));
        t.setMember ("description", std::string (def.description));
        t.setMember ("inputSchema", choc::json::parse (def.schemaJson));
        tools.addArrayElement (t);
    }

    return tools;
}

// ─────────────────────────────────────────────────
//  Tool handlers (all run on main thread)
// ─────────────────────────────────────────────────

static choc::value::Value toolListPlugins(const choc::value::Value& args)
{
    auto formatFilter = getStringArg (args, "format");
    auto& seq = AppModel::instance().sequencer();
    auto plugins = seq.engine()->pluginHost()->pluginCatalogEntries();

    auto arr = choc::value::createEmptyArray();
    for (auto p : plugins)           // non-const copy — catalog methods are not const
    {
        if (!formatFilter.empty() && p.format() != formatFilter)
            continue;
        auto obj = choc::value::createObject ("");
        obj.setMember ("pluginId", p.pluginId());
        obj.setMember ("displayName", p.displayName());
        obj.setMember ("format", p.format());
        obj.setMember ("vendorName", p.vendorName());
        arr.addArrayElement (obj);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("plugins", arr);
    return result;
}

static choc::value::Value toolListTracks(const choc::value::Value&)
{
    auto& seq = AppModel::instance().sequencer();
    auto& engine = *seq.engine();
    const auto& tracks = engine.tracks();

    auto arr = choc::value::createEmptyArray();
    for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i)
    {
        auto t = choc::value::createObject ("");
        t.setMember ("trackIndex", i);
        auto instances = choc::value::createEmptyArray();
        for (const auto& [instanceId, node] : tracks[static_cast<size_t>(i)]->graph().plugins())
        {
            if (!node) continue;
            auto inst = choc::value::createObject ("");
            inst.setMember ("instanceId", instanceId);
            auto* pluginInst = engine.getPluginInstance (instanceId);
            inst.setMember ("pluginName", pluginInst ? pluginInst->displayName() : std::string (""));
            instances.addArrayElement (inst);
        }
        t.setMember ("instances", instances);
        arr.addArrayElement (t);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("tracks", arr);
    return result;
}

static choc::value::Value toolCreateTrack(const choc::value::Value&)
{
    auto trackIndex = AppModel::instance().addTrack();
    auto result = choc::value::createObject ("");
    result.setMember ("trackIndex", trackIndex);
    return result;
}

static choc::value::Value toolAddPluginToTrack(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    auto pluginId   = getStringArg (args, "pluginId");
    auto format     = getStringArg (args, "format");

    if (trackIndex < 0 || pluginId.empty() || format.empty())
        throw std::invalid_argument ("trackIndex, pluginId and format are required");

    auto& model = AppModel::instance();
    AppModel::PluginInstanceConfig config{};

    std::atomic<int32_t> resultId{-1};
    std::atomic<bool> completed{false};
    std::string error;

    auto callback = [&](const AppModel::PluginInstanceResult& r) {
        if (r.instanceId < 0 || !r.error.empty())
            error = r.error.empty() ? "Failed to create instance" : r.error;
        else
            resultId = r.instanceId;
        completed = true;
    };

    model.instanceCreated.push_back (callback);
    model.createPluginInstanceAsync (format, pluginId, trackIndex, config);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (!completed && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    model.instanceCreated.pop_back();

    if (!completed)
        throw std::runtime_error ("Timeout creating plugin instance");
    if (!error.empty())
        throw std::runtime_error (error);

    auto result = choc::value::createObject ("");
    result.setMember ("instanceId", resultId.load());
    return result;
}

static choc::value::Value toolGetTimelineState(const choc::value::Value&)
{
    const auto& state = AppModel::instance().timeline();
    auto result = choc::value::createObject ("");
    result.setMember ("tempo", state.tempo);
    result.setMember ("timeSignatureNumerator", state.timeSignatureNumerator);
    result.setMember ("timeSignatureDenominator", state.timeSignatureDenominator);
    result.setMember ("isPlaying", state.isPlaying);
    result.setMember ("playheadSamples", choc::value::createInt64 (state.playheadPosition.samples));
    result.setMember ("loopEnabled", state.loopEnabled);
    result.setMember ("loopStartSamples", choc::value::createInt64 (state.loopStart.samples));
    result.setMember ("loopEndSamples", choc::value::createInt64 (state.loopEnd.samples));
    return result;
}

static choc::value::Value toolSetTempo(const choc::value::Value& args)
{
    double bpm = 120.0;
    if (args.isObject() && args.hasObjectMember ("bpm"))
        bpm = args["bpm"].get<double>();
    if (bpm <= 0.0)
        throw std::invalid_argument ("bpm must be positive");
    AppModel::instance().timeline().tempo = bpm;
    auto result = choc::value::createObject ("");
    result.setMember ("tempo", bpm);
    return result;
}

static choc::value::Value toolListClips(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    if (trackIndex < 0)
        throw std::invalid_argument ("trackIndex is required");

    auto tracks = AppModel::instance().getTimelineTracks();
    if (trackIndex >= static_cast<int32_t>(tracks.size()))
        throw std::out_of_range ("trackIndex out of range");

    const auto clips = tracks[static_cast<size_t>(trackIndex)]->clipManager().getAllClips();
    auto arr = choc::value::createEmptyArray();
    for (const auto& clip : clips)
    {
        auto obj = choc::value::createObject ("");
        obj.setMember ("clipId", clip.clipId);
        obj.setMember ("positionSamples", choc::value::createInt64 (clip.position.samples));
        obj.setMember ("durationSamples", choc::value::createInt64 (clip.durationSamples));
        obj.setMember ("name", clip.name);
        obj.setMember ("filepath", clip.filepath);
        obj.setMember ("clipType", clip.clipType == ClipType::Midi ? std::string ("midi") : std::string ("audio"));
        obj.setMember ("gain", clip.gain);
        obj.setMember ("muted", clip.muted);
        obj.setMember ("tempo", clip.clipTempo);
        obj.setMember ("tickResolution", static_cast<int32_t> (clip.tickResolution));
        arr.addArrayElement (obj);
    }

    auto result = choc::value::createObject ("");
    result.setMember ("clips", arr);
    return result;
}

static choc::value::Value toolAddMidiClip(const choc::value::Value& args)
{
    auto trackIndex      = getIntArg (args, "trackIndex");
    auto filepath        = getStringArg (args, "filepath");
    auto positionSamples = getInt64Arg (args, "positionSamples", 0);

    if (trackIndex < 0 || filepath.empty())
        throw std::invalid_argument ("trackIndex and filepath are required");

    TimelinePosition pos{};
    pos.samples = positionSamples;
    auto r = AppModel::instance().addMidiClipToTrack (trackIndex, pos, filepath);

    if (!r.success)
        throw std::runtime_error (r.error.empty() ? "Failed to add MIDI clip" : r.error);

    auto result = choc::value::createObject ("");
    result.setMember ("clipId", r.clipId);
    return result;
}

static choc::value::Value toolRemoveClip(const choc::value::Value& args)
{
    auto trackIndex = getIntArg (args, "trackIndex");
    auto clipId     = getIntArg (args, "clipId");

    if (trackIndex < 0 || clipId < 0)
        throw std::invalid_argument ("trackIndex and clipId are required");

    bool ok = AppModel::instance().removeClipFromTrack (trackIndex, clipId);
    auto result = choc::value::createObject ("");
    result.setMember ("success", ok);
    return result;
}

static choc::value::Value toolPlay(const choc::value::Value&)
{
    AppModel::instance().sequencer().engine()->startPlayback();
    auto result = choc::value::createObject ("");
    result.setMember ("playing", true);
    return result;
}

static choc::value::Value toolStop(const choc::value::Value&)
{
    AppModel::instance().sequencer().engine()->stopPlayback();
    auto result = choc::value::createObject ("");
    result.setMember ("playing", false);
    return result;
}


// ─────────────────────────────────────────────────
//  McpServer::Impl
// ─────────────────────────────────────────────────

struct McpServer::Impl {
    int port_;
    httplib::Server server_;
    std::thread thread_;
    std::unique_ptr<UapmdJSRuntime> jsRuntime_;   // created lazily on main thread

    struct PendingCall {
        std::function<std::string()> handler;
        std::promise<std::string> promise;
    };
    std::mutex mutex_;
    std::queue<std::shared_ptr<PendingCall>> queue_;

    // Called from HTTP thread — posts work and blocks until main thread processes it.
    std::string dispatchToMainThread(std::function<std::string()> fn)
    {
        auto call = std::make_shared<PendingCall>();
        call->handler = std::move (fn);
        auto future = call->promise.get_future();
        {
            std::lock_guard lock (mutex_);
            queue_.push (call);
        }
        return future.get();
    }

    // Called from main thread each frame.
    void processQueue()
    {
        while (true)
        {
            std::shared_ptr<PendingCall> call;
            {
                std::lock_guard lock (mutex_);
                if (queue_.empty()) break;
                call = queue_.front();
                queue_.pop();
            }
            try {
                call->promise.set_value (call->handler());
            } catch (...) {
                call->promise.set_exception (std::current_exception());
            }
        }
    }

    // ---- MCP protocol handlers ----

    std::string handleRequest(const std::string& body)
    {
        choc::value::Value msg;
        try {
            msg = choc::json::parse (body);
        } catch (...) {
            return jsonRpcError ({}, -32700, "Parse error");
        }

        auto id = choc::value::Value{};
        if (msg.isObject() && msg.hasObjectMember ("id"))
            id = choc::value::Value (msg["id"]);

        if (!msg.isObject() || !msg.hasObjectMember ("method"))
            return jsonRpcError (id, -32600, "Invalid Request");

        const auto method = msg["method"].get<std::string>();

        // Notifications (no id field) require no response.
        if (!msg.hasObjectMember ("id"))
            return {};

        if (method == "initialize")   return handleInitialize (id);
        if (method == "tools/list")   return handleToolsList (id);
        if (method == "tools/call")   return handleToolsCall (id, msg);

        return jsonRpcError (id, -32601, "Method not found: " + method);
    }

    std::string handleInitialize(const choc::value::Value& id)
    {
        auto result = choc::value::createObject ("");
        result.setMember ("protocolVersion", std::string ("2024-11-05"));

        auto caps = choc::value::createObject ("");
        caps.setMember ("tools", choc::value::createObject (""));
        result.setMember ("capabilities", caps);

        auto info = choc::value::createObject ("");
        info.setMember ("name", std::string ("uapmd"));
        info.setMember ("version", std::string ("1.0.0"));
        result.setMember ("serverInfo", info);

        return jsonRpcResult (id, result);
    }

    std::string handleToolsList(const choc::value::Value& id)
    {
        auto result = choc::value::createObject ("");
        result.setMember ("tools", buildToolDefinitions());
        return jsonRpcResult (id, result);
    }

    // Lazily initialise the JS runtime and bootstrap all AppJsLib scripts.
    // Safe to call multiple times — no-op after the first call.
    void ensureJSRuntime()
    {
        if (jsRuntime_)
            return;
        jsRuntime_ = std::make_unique<UapmdJSRuntime>();
        for (auto& [filename, data] : ResEmbed::getCategory ("AppJsLib"))
        {
            std::string src (reinterpret_cast<const char*> (data.data()), data.size());
            try { jsRuntime_->context().evaluateExpression (src); }
            catch (const std::exception& e) {
                std::cerr << "[MCP] Failed to bootstrap " << filename << ": " << e.what() << std::endl;
            }
        }
    }

    // Execute a JS snippet and return the string representation of the result.
    // Throws on evaluation error.
    std::string evalScript(const std::string& code)
    {
        ensureJSRuntime();
        auto evalResult = jsRuntime_->context().evaluateExpression (code);
        return evalResult.isVoid() ? std::string ("undefined")
                                   : choc::json::toString (evalResult);
    }

    std::string handleToolsCall(const choc::value::Value& id, const choc::value::Value& msg)
    {
        if (!msg.hasObjectMember ("params"))
            return jsonRpcError (id, -32602, "Missing params");

        const auto& params = msg["params"];
        if (!params.hasObjectMember ("name"))
            return jsonRpcError (id, -32602, "Missing tool name");

        const auto toolName = params["name"].get<std::string>();

        // Extract arguments as an owned Value (not a view into msg).
        auto args = params.hasObjectMember ("arguments")
            ? choc::value::Value (params["arguments"])
            : choc::value::createObject ("");

        try
        {
            choc::value::Value toolResult;

            if      (toolName == "list_plugins")        toolResult = toolListPlugins (args);
            else if (toolName == "list_tracks")         toolResult = toolListTracks (args);
            else if (toolName == "create_track")        toolResult = toolCreateTrack (args);
            else if (toolName == "add_plugin_to_track") toolResult = toolAddPluginToTrack (args);
            else if (toolName == "get_timeline_state")  toolResult = toolGetTimelineState (args);
            else if (toolName == "set_tempo")           toolResult = toolSetTempo (args);
            else if (toolName == "list_clips")          toolResult = toolListClips (args);
            else if (toolName == "add_midi_clip")       toolResult = toolAddMidiClip (args);
            else if (toolName == "remove_clip")         toolResult = toolRemoveClip (args);
            else if (toolName == "play")                toolResult = toolPlay (args);
            else if (toolName == "stop")                toolResult = toolStop (args);
            else if (toolName == "run_script")
            {
                auto code = getStringArg (args, "code");
                if (code.empty())
                    throw std::invalid_argument ("code is required");
                auto output = evalScript (code);
                toolResult = choc::value::createObject ("");
                toolResult.setMember ("output", output);
            }
            else
                return jsonRpcError (id, -32601, "Unknown tool: " + toolName);

            auto result = choc::value::createObject ("");
            result.setMember ("content", textContent (choc::json::toString (toolResult)));
            return jsonRpcResult (id, result);
        }
        catch (const std::exception& e)
        {
            auto result = choc::value::createObject ("");
            result.setMember ("content", textContent (std::string ("Error: ") + e.what()));
            result.setMember ("isError", true);
            return jsonRpcResult (id, result);
        }
    }
};

// ─────────────────────────────────────────────────
//  McpServer public API
// ─────────────────────────────────────────────────

McpServer::McpServer(int port)
    : impl_ (std::make_unique<Impl>())
{
    impl_->port_ = port;
}

McpServer::~McpServer()
{
    stop();
}

void McpServer::start()
{
    impl_->server_.Post ("/mcp", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto body = req.body;
        std::string response = impl_->dispatchToMainThread ([this, body] {
            return impl_->handleRequest (body);
        });

        if (response.empty())
            res.status = 202;   // notification — no body
        else
            res.set_content (response, "application/json");
    });

    // Bare JS REPL endpoint: POST /eval  body=<script>  → plain-text result.
    // Simpler than going through JSON-RPC; useful for quick ad-hoc scripting.
    impl_->server_.Post ("/eval", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto code = req.body;
        try {
            std::string output = impl_->dispatchToMainThread ([this, code] {
                return impl_->evalScript (code);
            });
            res.set_content (output, "text/plain");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content (e.what(), "text/plain");
        }
    });

    const auto port = impl_->port_;
    impl_->thread_ = std::thread ([this, port] {
        std::cout << "[MCP] Listening on http://127.0.0.1:" << port << "/mcp  (JS eval: /eval)" << std::endl;
        impl_->server_.listen ("127.0.0.1", port);
    });
}

void McpServer::stop()
{
    impl_->server_.stop();
    if (impl_->thread_.joinable())
        impl_->thread_.join();
}

int McpServer::port() const
{
    return impl_->port_;
}

void McpServer::processMainThreadQueue()
{
    impl_->processQueue();
}

} // namespace uapmd
