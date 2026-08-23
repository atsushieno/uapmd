#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Just enough ONNX to get the weights out.
//
// The Basic Pitch model is used as a weight file, not as a graph: the network
// is written directly in C++ against the layer layout, so nothing here needs
// to understand nodes, attributes or operators. That keeps this to a protobuf
// wire-format reader for four fields, instead of a dependency on libprotobuf
// or onnxruntime -- neither of which this project could carry to WebAssembly
// and Android without a fight.
namespace uapmd_basic_pitch {

struct OnnxTensor {
    std::string name;
    std::vector<int64_t> dims;
    std::vector<float> data;

    size_t elementCount() const;
};

// Reads every float32 initializer in an ONNX model. Initializers of other
// types are skipped: in this model they are shape constants, which a
// hand-written network does not need. Returns false and fills `error` when the
// buffer is not a model this reader can walk.
bool readOnnxFloatInitializers(std::span<const uint8_t> model,
                               std::vector<OnnxTensor>& tensors,
                               std::string& error);

// The Basic Pitch model, embedded at build time from the ONNX file CMake
// downloads. Decoded on first call and cached.
std::span<const uint8_t> embeddedModel();

} // namespace uapmd_basic_pitch
