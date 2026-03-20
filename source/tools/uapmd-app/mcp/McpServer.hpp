#pragma once

#include <functional>
#include <memory>
#include <string>

namespace uapmd {

/**
 * Embedded MCP (Model Context Protocol) server over HTTP.
 *
 * Starts a lightweight HTTP server on the given port.  Incoming
 * JSON-RPC 2.0 requests are queued and dispatched on the main thread
 * by calling processMainThreadQueue() from the render loop each frame.
 *
 * Enable with: uapmd-app --mcp-server [port]   (default port 37373)
 */
class McpServer {
public:
    explicit McpServer(int port);
    ~McpServer();

    void start();
    void stop();
    int port() const;

    /** Call once per frame from the main/GUI thread to process queued tool calls. */
    void processMainThreadQueue();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uapmd
