#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace remidy_tooling::ipc {

class TcpSocket {
public:
    TcpSocket();
    explicit TcpSocket(intptr_t handle);
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    ~TcpSocket();

    bool connect(const std::string& host, uint16_t port);
    bool sendAll(const void* data, size_t bytes);
    bool receiveAll(void* data, size_t bytes);
    bool setNoDelay(bool enable);
    bool setReadTimeout(int milliseconds);
    bool setWriteTimeout(int milliseconds);
    int waitForRead(int timeoutMilliseconds) const;
    bool valid() const;
    void close();
    intptr_t nativeHandle() const { return handle_; }

private:
    intptr_t handle_{-1};
};

class TcpServer {
public:
    TcpServer();
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    bool listen(uint16_t portHint = 0);
    std::optional<TcpSocket> accept(int timeoutMilliseconds);
    uint16_t port() const { return port_; }
    void close();

private:
    intptr_t handle_{-1};
    uint16_t port_{0};
};

bool ensureSocketLayerInitialized();

} // namespace remidy_tooling::ipc
