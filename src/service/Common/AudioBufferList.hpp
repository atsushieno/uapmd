#pragma once
#include <cstdint>

class AudioBufferList {
public:
    float* getFloatBufferForChannel(int32_t channel);
    double* getDoubleBufferForChannel(int32_t channel);
    int32_t size();
};
