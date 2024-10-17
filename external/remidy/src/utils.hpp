#pragma once

#include <string>
#include <functional>

// string-to-and-from-hex converters
std::string hexBinaryToString(const char* s, const size_t size, const bool capital = false);
std::string stringToHexBinary(std::string s);
