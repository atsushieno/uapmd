#pragma once

// remidy-gui library umbrella header
// Includes all public headers from the remidy-gui module

#include <remidy-gui/priv/ContainerWindow.hpp>
#if defined(__ANDROID__)
#include <remidy-gui/priv/AndroidContainerWindow.hpp>
#else
#include <remidy-gui/priv/GLContextGuard.hpp>
#endif
