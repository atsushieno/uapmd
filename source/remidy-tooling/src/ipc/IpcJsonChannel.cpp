#include "IpcJsonChannel.hpp"

#include <choc/text/choc_JSON.h>
#if _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace remidy_tooling::ipc {

namespace {

constexpr uint32_t kProtocolVersion = 1;

choc::value::Value toValue(const IpcMessage& message) {
    auto obj = choc::value::createObject("IpcMessage");
    obj.setMember("type", message.type);
    if (!message.requestId.empty())
        obj.setMember("id", message.requestId);
    if (!message.payload.isVoid())
        obj.setMember("payload", message.payload);
    obj.setMember("version", static_cast<int32_t>(kProtocolVersion));
    return obj;
}

std::optional<IpcMessage> fromValue(const choc::value::Value& value) {
    auto view = value.getView();
    auto typeMember = view["type"];
    if (typeMember.isVoid())
        return std::nullopt;
    IpcMessage message;
    message.type = typeMember.toString();
    auto id = view["id"];
    message.requestId = id.isVoid() ? std::string{} : id.toString();
    auto payload = view["payload"];
    if (!payload.isVoid())
        message.payload = payload;
    return message;
}

} // namespace

IpcJsonChannel::IpcJsonChannel(TcpSocket socket)
    : socket_(std::move(socket)) {}

bool IpcJsonChannel::send(const IpcMessage& message) {
    if (!socket_.valid())
        return false;
    auto serialized = choc::json::toString(toValue(message), false);
    uint32_t length = htonl(static_cast<uint32_t>(serialized.size()));
    if (!socket_.sendAll(&length, sizeof(length)))
        return false;
    return socket_.sendAll(serialized.data(), serialized.size());
}

std::optional<IpcMessage> IpcJsonChannel::receive(int timeoutMilliseconds) {
    bool timedOut = false;
    return receive(timeoutMilliseconds, timedOut);
}

std::optional<IpcMessage> IpcJsonChannel::receive(int timeoutMilliseconds, bool& timedOut) {
    timedOut = false;
    if (!socket_.valid())
        return std::nullopt;
    if (timeoutMilliseconds >= 0) {
        auto waitResult = socket_.waitForRead(timeoutMilliseconds);
        if (waitResult == 0) {
            timedOut = true;
            return std::nullopt;
        }
        if (waitResult < 0)
            return std::nullopt;
    }
    uint32_t length = 0;
    if (!socket_.receiveAll(&length, sizeof(length)))
        return std::nullopt;
    length = ntohl(length);
    if (length == 0)
        return std::nullopt;
    std::string buffer(length, '\0');
    if (!socket_.receiveAll(buffer.data(), buffer.size()))
        return std::nullopt;
    try {
        auto value = choc::json::parse(buffer);
        return fromValue(value);
    } catch (...) {
        return std::nullopt;
    }
}

void IpcJsonChannel::close() {
    socket_.close();
}

} // namespace remidy_tooling::ipc
