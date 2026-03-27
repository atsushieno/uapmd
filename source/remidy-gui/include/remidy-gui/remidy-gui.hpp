#pragma once

// remidy-gui library umbrella header
// Includes all public headers from the remidy-gui module

#include <remidy-gui/detail/ContainerWindow.hpp>
#if defined(__ANDROID__)
#include <remidy-gui/detail/AndroidContainerWindow.hpp>
#else
#include <remidy-gui/detail/GLContextGuard.hpp>
#endif
