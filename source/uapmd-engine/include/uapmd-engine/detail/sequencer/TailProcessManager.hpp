#pragma once

#include <cstdint>
#include <functional>

namespace uapmd {

class TailProcessManager {
public:
    using TransportQuietListenerId = uint64_t;
    using TransportQuietListener = std::function<void()>;

    virtual ~TailProcessManager() = default;

    virtual bool isTransportQuiet() const = 0;
    virtual TransportQuietListenerId addTransportQuietListener(
        TransportQuietListener listener) = 0;
    virtual void removeTransportQuietListener(
        TransportQuietListenerId listenerId) = 0;
};

} // namespace uapmd
