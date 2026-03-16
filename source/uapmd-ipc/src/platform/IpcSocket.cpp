// Platform-transparent socket I/O helpers declared in IpcProtocol.hpp.
//
// Uses POSIX send/recv on all platforms; Windows AF_UNIX sockets
// support the same API since Windows 10 build 17063.

#include <uapmd-ipc/IpcProtocol.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using ssize_t = SSIZE_T;
    #define SOCK_CLOSE closesocket
#else
    #include <sys/socket.h>
    #include <unistd.h>
    #define SOCK_CLOSE close
#endif

#include <cstring>

namespace uapmd::ipc {

bool ipcReadExact(int fd, void* buf, size_t n) {
    auto* p = static_cast<char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
#ifdef _WIN32
        int got = ::recv(static_cast<SOCKET>(fd), p, static_cast<int>(remaining), 0);
#else
        ssize_t got = ::recv(fd, p, remaining, MSG_WAITALL);
#endif
        if (got <= 0)
            return false;
        p         += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

bool ipcWriteExact(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
#ifdef _WIN32
        int sent = ::send(static_cast<SOCKET>(fd), p, static_cast<int>(remaining), 0);
#else
        ssize_t sent = ::send(fd, p, remaining, MSG_NOSIGNAL);
#endif
        if (sent <= 0)
            return false;
        p         += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

bool ipcRecvMsg(int fd, IpcHeader& hdr, std::vector<uint8_t>& payload) {
    if (!ipcReadExact(fd, &hdr, sizeof(hdr)))
        return false;
    payload.resize(hdr.size);
    if (hdr.size > 0 && !ipcReadExact(fd, payload.data(), hdr.size))
        return false;
    return true;
}

bool ipcSendMsg(int fd, IpcMsgType type, uint64_t reqId, const std::vector<uint8_t>& payload) {
    IpcHeader hdr{};
    hdr.type   = static_cast<uint32_t>(type);
    hdr.size   = static_cast<uint32_t>(payload.size());
    hdr.req_id = reqId;
    if (!ipcWriteExact(fd, &hdr, sizeof(hdr)))
        return false;
    if (!payload.empty() && !ipcWriteExact(fd, payload.data(), payload.size()))
        return false;
    return true;
}

} // namespace uapmd::ipc
