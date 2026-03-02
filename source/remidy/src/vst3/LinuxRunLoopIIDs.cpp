#include "pluginterfaces/gui/iplugview.h"

// The Steinberg SDK only defines the Linux run loop IIDs when SMTG_OS_LINUX is set.
// We use these interfaces on every platform (the mac run loop is implemented
// through the Linux-style callbacks), so provide the missing definitions when
// building elsewhere to satisfy the linker.
#if !SMTG_OS_LINUX
namespace Steinberg {
namespace Linux {
DEF_CLASS_IID (IEventHandler)
DEF_CLASS_IID (ITimerHandler)
DEF_CLASS_IID (IRunLoop)
} // namespace Linux
} // namespace Steinberg
#endif
