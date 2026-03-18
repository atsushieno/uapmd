#pragma once

#include <optional>
#include <string>

#include <choc/text/choc_JSON.h>

namespace remidy_tooling::ipc {

struct IpcMessage {
    std::string type;
    std::string requestId;
    choc::value::Value payload;
};

} // namespace remidy_tooling::ipc
