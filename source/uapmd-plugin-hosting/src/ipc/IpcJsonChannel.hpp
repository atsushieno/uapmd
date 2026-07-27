#pragma once

#include <chrono>
#include <optional>

#include "IpcMessage.hpp"
#include "TcpSocket.hpp"

namespace remidy_tooling::ipc {

class IpcJsonChannel {
public:
    explicit IpcJsonChannel(TcpSocket socket);
    IpcJsonChannel(const IpcJsonChannel&) = delete;
    IpcJsonChannel& operator=(const IpcJsonChannel&) = delete;
    IpcJsonChannel(IpcJsonChannel&&) noexcept = default;
    IpcJsonChannel& operator=(IpcJsonChannel&&) noexcept = default;

    bool send(const IpcMessage& message);
    std::optional<IpcMessage> receive(int timeoutMilliseconds);
    std::optional<IpcMessage> receive(int timeoutMilliseconds, bool& timedOut);
    void close();
    bool valid() const { return socket_.valid(); }

private:
    TcpSocket socket_;
};

} // namespace remidy_tooling::ipc
