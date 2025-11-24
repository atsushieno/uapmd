#include <AudioToolbox/AudioToolbox.h>
#include <functional>
#include <string>

bool audioUnitHasIO(AudioUnit audioUnit, AudioUnitScope scope);

std::string retrieveCFStringRelease(const std::function<void(CFStringRef&)>&& retriever);

std::string cfStringToString(CFStringRef s);
