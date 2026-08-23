#include "OnnxWeights.hpp"

#include <cstring>
#include <numeric>

namespace uapmd_basic_pitch {

namespace {

// Protobuf wire types and the field numbers this reader cares about.
constexpr uint32_t kWireVarint = 0;
constexpr uint32_t kWireFixed64 = 1;
constexpr uint32_t kWireLengthDelimited = 2;
constexpr uint32_t kWireFixed32 = 5;

constexpr uint32_t kModelGraph = 7;        // ModelProto.graph
constexpr uint32_t kGraphInitializer = 5;  // GraphProto.initializer
constexpr uint32_t kTensorDims = 1;        // TensorProto.dims
constexpr uint32_t kTensorDataType = 2;    // TensorProto.data_type
constexpr uint32_t kTensorName = 8;        // TensorProto.name
constexpr uint32_t kTensorRawData = 9;     // TensorProto.raw_data

constexpr uint64_t kTensorProtoFloat = 1;  // TensorProto.DataType.FLOAT

// A cursor over one protobuf message. Every read is bounds-checked, because
// the buffer arrives from a download and a truncated one must fail rather
// than walk off the end.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    bool done() const { return failed_ || offset_ >= bytes_.size(); }
    bool failed() const { return failed_; }

    bool readVarint(uint64_t& value) {
        value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (offset_ >= bytes_.size())
                return fail();
            const auto byte = bytes_[offset_++];
            value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
            if (!(byte & 0x80u))
                return true;
        }
        return fail();
    }

    // Reads the next field's tag, leaving the cursor on its payload.
    bool readTag(uint32_t& fieldNumber, uint32_t& wireType) {
        uint64_t key = 0;
        if (!readVarint(key))
            return false;
        fieldNumber = static_cast<uint32_t>(key >> 3);
        wireType = static_cast<uint32_t>(key & 0x7u);
        return true;
    }

    bool readBytes(std::span<const uint8_t>& out) {
        uint64_t length = 0;
        if (!readVarint(length))
            return false;
        if (length > bytes_.size() - offset_)
            return fail();
        out = bytes_.subspan(offset_, static_cast<size_t>(length));
        offset_ += static_cast<size_t>(length);
        return true;
    }

    // Steps over a field whose contents this reader has no use for.
    bool skip(uint32_t wireType) {
        switch (wireType) {
            case kWireVarint: {
                uint64_t ignored = 0;
                return readVarint(ignored);
            }
            case kWireFixed64:
                return advance(8);
            case kWireLengthDelimited: {
                std::span<const uint8_t> ignored;
                return readBytes(ignored);
            }
            case kWireFixed32:
                return advance(4);
            default:
                return fail();
        }
    }

private:
    bool advance(size_t count) {
        if (count > bytes_.size() - offset_)
            return fail();
        offset_ += count;
        return true;
    }

    bool fail() {
        failed_ = true;
        return false;
    }

    std::span<const uint8_t> bytes_;
    size_t offset_{0};
    bool failed_{false};
};

// `dims` is emitted either as repeated varints or as one packed field,
// depending on the writer, so both spellings have to be accepted.
bool readDims(Reader& reader, uint32_t wireType, std::vector<int64_t>& dims) {
    if (wireType == kWireVarint) {
        uint64_t dim = 0;
        if (!reader.readVarint(dim))
            return false;
        dims.push_back(static_cast<int64_t>(dim));
        return true;
    }
    if (wireType != kWireLengthDelimited)
        return reader.skip(wireType);

    std::span<const uint8_t> packed;
    if (!reader.readBytes(packed))
        return false;
    Reader inner{packed};
    while (!inner.done()) {
        uint64_t dim = 0;
        if (!inner.readVarint(dim))
            return false;
        dims.push_back(static_cast<int64_t>(dim));
    }
    return !inner.failed();
}

bool readTensor(std::span<const uint8_t> bytes, OnnxTensor& tensor, bool& isFloat) {
    Reader reader{bytes};
    std::span<const uint8_t> raw;
    uint64_t dataType = 0;
    while (!reader.done()) {
        uint32_t field = 0;
        uint32_t wireType = 0;
        if (!reader.readTag(field, wireType))
            return false;
        switch (field) {
            case kTensorDims:
                if (!readDims(reader, wireType, tensor.dims))
                    return false;
                break;
            case kTensorDataType:
                if (!reader.readVarint(dataType))
                    return false;
                break;
            case kTensorName: {
                std::span<const uint8_t> name;
                if (!reader.readBytes(name))
                    return false;
                tensor.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
                break;
            }
            case kTensorRawData:
                if (!reader.readBytes(raw))
                    return false;
                break;
            default:
                if (!reader.skip(wireType))
                    return false;
                break;
        }
    }
    if (reader.failed())
        return false;

    isFloat = dataType == kTensorProtoFloat;
    if (!isFloat)
        return true;
    // raw_data holds little-endian float32, which is what every target this
    // builds for uses natively.
    tensor.data.resize(raw.size() / sizeof(float));
    if (!tensor.data.empty())
        std::memcpy(tensor.data.data(), raw.data(), tensor.data.size() * sizeof(float));
    return true;
}

} // namespace

size_t OnnxTensor::elementCount() const {
    if (dims.empty())
        return data.size();
    return static_cast<size_t>(std::accumulate(
        dims.begin(), dims.end(), int64_t{1}, std::multiplies<int64_t>{}));
}

bool readOnnxFloatInitializers(std::span<const uint8_t> model,
                               std::vector<OnnxTensor>& tensors,
                               std::string& error) {
    tensors.clear();

    Reader modelReader{model};
    std::span<const uint8_t> graph;
    bool foundGraph = false;
    while (!modelReader.done()) {
        uint32_t field = 0;
        uint32_t wireType = 0;
        if (!modelReader.readTag(field, wireType))
            break;
        if (field == kModelGraph && wireType == kWireLengthDelimited) {
            if (!modelReader.readBytes(graph))
                break;
            foundGraph = true;
            break;
        }
        if (!modelReader.skip(wireType))
            break;
    }
    if (!foundGraph) {
        error = "ONNX model has no graph";
        return false;
    }

    Reader graphReader{graph};
    while (!graphReader.done()) {
        uint32_t field = 0;
        uint32_t wireType = 0;
        if (!graphReader.readTag(field, wireType))
            break;
        if (field != kGraphInitializer || wireType != kWireLengthDelimited) {
            if (!graphReader.skip(wireType))
                break;
            continue;
        }
        std::span<const uint8_t> initializer;
        if (!graphReader.readBytes(initializer))
            break;
        OnnxTensor tensor;
        bool isFloat = false;
        if (!readTensor(initializer, tensor, isFloat)) {
            error = "malformed ONNX initializer";
            return false;
        }
        if (isFloat)
            tensors.push_back(std::move(tensor));
    }
    if (graphReader.failed()) {
        error = "malformed ONNX graph";
        return false;
    }
    if (tensors.empty()) {
        error = "ONNX model has no float initializers";
        return false;
    }
    return true;
}

} // namespace uapmd_basic_pitch
