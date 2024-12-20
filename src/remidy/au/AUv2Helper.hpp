#include <AudioToolbox/AudioToolbox.h>

bool audioUnitHasIO(AudioUnit audioUnit, AudioUnitScope scope);

std::string retrieveCFStringRelease(const std::function<void(CFStringRef&)>&& retriever);
