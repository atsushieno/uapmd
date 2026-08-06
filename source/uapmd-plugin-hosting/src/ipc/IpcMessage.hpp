#pragma once

#include <optional>
#include <string>

#include <choc/text/choc_JSON.h>

namespace uapmd_plugin_hosting::ipc {

struct IpcMessage {
    std::string type;
    std::string requestId;
    choc::value::Value payload;
};

} // namespace uapmd_plugin_hosting::ipc
