#pragma once
#include <cstdint>
#include <remidy/remidy.hpp>

typedef int32_t uapmd_status_t;
typedef uint32_t uapmd_ump_t;
typedef int64_t uapmd_timestamp_t;

typedef void* uapmd_device_t;
typedef remidy::AudioBufferList AudioBufferList;
typedef remidy::MidiSequence MidiSequence;
typedef remidy::AudioProcessContext AudioProcessContext;
