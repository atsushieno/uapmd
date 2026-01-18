#pragma once

#include <optional>
#include <string>

#include <libremidi/libremidi.hpp>

namespace uapmd::detail {

std::optional<libremidi::API> resolveLibremidiUmpApi(const std::string& apiName);

}
