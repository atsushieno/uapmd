
#include "utils.hpp"


// These functions are mostly based on https://stackoverflow.com/a/35599923/1465645
std::string hexBinaryToString(const char* s, const size_t size, const bool capital) {
    std::string ret{};
    ret.resize(size * 2);
    const size_t a = capital ? 'A' - 1 : 'a' - 1;

    for (size_t i = 0, c = s[0] & 0xFF; i < ret.size(); c = s[i / 2] & 0xFF) {
        ret[i++] = c > 0x9F ? (c / 16 - 9) | a : c / 16 | '0';
        ret[i++] = (c & 0xF) > 9 ? (c % 16 - 9) | a : c % 16 | '0';
    }
    return ret;
}
std::string stringToHexBinary(std::string s) {
    std::string ret{};
    ret.resize((s.size() + 1) / 2);
    for (size_t i = 0, j = 0; i < ret.size(); i++, j++) {
        ret[i] = (s[j] & '@' ? s[j] + 9 : s[j]) << 4, j++;
        ret[i] |= (s[j] & '@' ? s[j] + 9 : s[j]) & 0xF;
    }
    return ret;
}
