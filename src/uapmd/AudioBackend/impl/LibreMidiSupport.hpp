#pragma once

#include <optional>
#include <string>

#include <libremidi/libremidi-c.h>

namespace uapmd::detail {

std::optional<libremidi_api> resolveLibremidiUmpApi(const std::string& apiName);

}
