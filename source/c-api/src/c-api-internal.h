/* Internal shared declarations for uapmd C-API translation units */
#pragma once

#include <mutex>
#include <memory>
#include <unordered_map>

namespace uapmd { class AudioFileReader; }

/* AudioFileReader ownership registry — defined in uapmd-c-data.cpp */
extern std::mutex s_reader_mutex;
extern std::unordered_map<uapmd::AudioFileReader*, std::unique_ptr<uapmd::AudioFileReader>> s_owned_readers;
