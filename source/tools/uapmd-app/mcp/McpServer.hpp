#pragma once

#include <functional>
#include <memory>
#include <string>

namespace uapmd {

enum class McpConnectionMode {
    Server,   ///< Embedded HTTP server (desktop only, UAPMD_MCP_HAS_HTTP_SERVER)
    Client,   ///< Outbound WebSocket client to an external MCP relay
};

enum class McpConnectionState {
    Idle,         ///< Not started, or cleanly stopped
    Connecting,   ///< WebSocket handshake in progress
    Connected,    ///< Active and ready
    Error,        ///< Last connection attempt failed
};

/**
 * MCP (Model Context Protocol) transport layer.
 *
 * Two modes:
 *   Server mode  — embeds an HTTP/JSON-RPC server on localhost (desktop only).
 *                  Constructed with McpServer(int port).
 *   Client mode  — opens an outbound WebSocket connection to an external relay.
 *                  Constructed with McpServer(std::string relayUrl, bool autoReconnect).
 *
 * In both modes, incoming JSON-RPC requests are queued and dispatched on the
 * main thread by calling processMainThreadQueue() from the render loop each frame.
 */
class McpServer {
public:
    /// Server mode: listen on localhost:port.
    explicit McpServer(int port);

    /// Client mode: connect to relayUrl (e.g. "ws://192.168.1.5:8765/mcp").
    explicit McpServer(std::string relayUrl, bool autoReconnect = true);

    ~McpServer();

    void start();
    void stop();

    McpConnectionMode  mode()            const;
    McpConnectionState connectionState() const;
    std::string        statusMessage()   const;
    int                port()            const;   ///< Server mode only

    /** Call once per frame from the main/GUI thread to process queued tool calls. */
    void processMainThreadQueue();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uapmd
