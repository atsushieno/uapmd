#pragma once

#include <memory>

#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd::builtin {

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createGainNodeFactory();

}
