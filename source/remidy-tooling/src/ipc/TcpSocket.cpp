#include "TcpSocket.hpp"

#include <chrono>
#include <system_error>

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace remidy_tooling::ipc {

namespace {

#if _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidHandle = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidHandle = -1;
#endif

SocketHandle toHandle(intptr_t value) {
#if _WIN32
    return static_cast<SocketHandle>(value);
#else
    return static_cast<SocketHandle>(value);
#endif
}

intptr_t fromHandle(SocketHandle handle) {
    return static_cast<intptr_t>(handle);
}

void closeHandle(SocketHandle handle) {
    if (handle == kInvalidHandle)
        return;
#if _WIN32
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

bool waitForSocket(SocketHandle handle, bool forWrite, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(handle, &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    auto result = ::select(static_cast<int>(handle + 1),
                           forWrite ? nullptr : &set,
                           forWrite ? &set : nullptr,
                           nullptr,
                           timeoutMs >= 0 ? &tv : nullptr);
    return result > 0;
}

class SocketInitializer {
public:
    SocketInitializer() {
#if _WIN32
        WSADATA wsaData;
        initialized_ = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
        initialized_ = true;
#endif
    }

    ~SocketInitializer() {
#if _WIN32
        if (initialized_)
            WSACleanup();
#endif
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_{false};
};

SocketInitializer& globalInitializer() {
    static SocketInitializer initializer;
    return initializer;
}

} // namespace

bool ensureSocketLayerInitialized() {
    return globalInitializer().initialized();
}

TcpSocket::TcpSocket() = default;

TcpSocket::TcpSocket(intptr_t handle)
    : handle_(handle) {}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept {
    handle_ = other.handle_;
    other.handle_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
    return *this;
}

TcpSocket::~TcpSocket() {
    close();
}

bool TcpSocket::connect(const std::string& host, uint16_t port) {
    close();
    if (!ensureSocketLayerInitialized())
        return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    auto portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0)
        return false;

    SocketHandle socketHandle = kInvalidHandle;
    for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        socketHandle = ::socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (socketHandle == kInvalidHandle)
            continue;
        if (::connect(socketHandle, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0)
            break;
        closeHandle(socketHandle);
        socketHandle = kInvalidHandle;
    }
    ::freeaddrinfo(result);

    if (socketHandle == kInvalidHandle)
        return false;

    handle_ = fromHandle(socketHandle);
    setNoDelay(true);
    return true;
}

bool TcpSocket::sendAll(const void* data, size_t bytes) {
    auto handle = toHandle(handle_);
    if (handle == kInvalidHandle)
        return false;
    auto* cursor = static_cast<const char*>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
#if _WIN32
        auto sent = ::send(handle, cursor, static_cast<int>(remaining), 0);
#else
        auto sent = ::send(handle, cursor, remaining, 0);
#endif
        if (sent <= 0)
            return false;
        cursor += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

bool TcpSocket::receiveAll(void* data, size_t bytes) {
    auto handle = toHandle(handle_);
    if (handle == kInvalidHandle)
        return false;
    auto* cursor = static_cast<char*>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
#if _WIN32
        auto received = ::recv(handle, cursor, static_cast<int>(remaining), 0);
#else
        auto received = ::recv(handle, cursor, remaining, 0);
#endif
        if (received <= 0)
            return false;
        cursor += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

bool TcpSocket::setNoDelay(bool enable) {
    auto handle = toHandle(handle_);
    if (handle == kInvalidHandle)
        return false;
    int flag = enable ? 1 : 0;
    return ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&flag),
                        sizeof(flag)) == 0;
}

bool TcpSocket::setReadTimeout(int milliseconds) {
    auto handle = toHandle(handle_);
    if (handle == kInvalidHandle)
        return false;
#if _WIN32
    DWORD timeout = static_cast<DWORD>(milliseconds);
    return ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&timeout),
                        sizeof(timeout)) == 0;
#else
    timeval tv{
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000
    };
    return ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                        &tv, sizeof(tv)) == 0;
#endif
}

bool TcpSocket::setWriteTimeout(int milliseconds) {
    auto handle = toHandle(handle_);
    if (handle == kInvalidHandle)
        return false;
#if _WIN32
    DWORD timeout = static_cast<DWORD>(milliseconds);
    return ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&timeout),
                        sizeof(timeout)) == 0;
#else
    timeval tv{
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000
    };
    return ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO,
                        &tv, sizeof(tv)) == 0;
#endif
}

int TcpSocket::waitForRead(int milliseconds) const {
    auto handle = toHandle(handle_);
    if (handle == kInvalidHandle)
        return -1;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(handle, &readSet);
    timeval tv{
        .tv_sec = milliseconds < 0 ? 0 : milliseconds / 1000,
        .tv_usec = milliseconds < 0 ? 0 : (milliseconds % 1000) * 1000
    };
    auto result = ::select(static_cast<int>(handle + 1),
                           &readSet,
                           nullptr,
                           nullptr,
                           milliseconds >= 0 ? &tv : nullptr);
    if (result > 0)
        return 1;
    if (result == 0)
        return 0;
    return -1;
}

bool TcpSocket::valid() const {
    return handle_ != -1;
}

void TcpSocket::close() {
    if (!valid())
        return;
    closeHandle(toHandle(handle_));
    handle_ = -1;
}

TcpServer::TcpServer() = default;

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::listen(uint16_t portHint) {
    close();
    if (!ensureSocketLayerInitialized())
        return false;

    SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidHandle)
        return false;

    int enable = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&enable),
                 sizeof(enable));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(portHint);

    if (::bind(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeHandle(handle);
        return false;
    }

    if (::listen(handle, 1) != 0) {
        closeHandle(handle);
        return false;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        closeHandle(handle);
        return false;
    }

    handle_ = fromHandle(handle);
    port_ = ntohs(bound.sin_port);
    return true;
}

std::optional<TcpSocket> TcpServer::accept(int timeoutMilliseconds) {
    auto serverHandle = toHandle(handle_);
    if (serverHandle == kInvalidHandle)
        return std::nullopt;

    if (timeoutMilliseconds >= 0 && !waitForSocket(serverHandle, false, timeoutMilliseconds))
        return std::nullopt;

    sockaddr_in clientAddr{};
    socklen_t len = sizeof(clientAddr);
    auto clientHandle = ::accept(serverHandle, reinterpret_cast<sockaddr*>(&clientAddr), &len);
    if (clientHandle == kInvalidHandle)
        return std::nullopt;

    TcpSocket socket(fromHandle(clientHandle));
    socket.setNoDelay(true);
    return socket;
}

void TcpServer::close() {
    if (handle_ == -1)
        return;
    closeHandle(toHandle(handle_));
    handle_ = -1;
    port_ = 0;
}

} // namespace remidy_tooling::ipc
