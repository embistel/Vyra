#pragma once

#include "core/common.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace vyra {

class Backend;

using Sink = std::function<void(const std::uint8_t *data, std::size_t size)>;

// Incremental decoder for one coded stream (a 7z folder or a zip entry).
// feed() may be called with arbitrarily sized pieces; decoded bytes are
// passed to the sink in order. The last call has end == true.
class StreamDecoder {
public:
    virtual ~StreamDecoder() = default;
    virtual void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) = 0;
};

struct DecoderSpec {
    Method method = Method::Store;
    std::vector<std::uint8_t> properties;
    std::uint64_t unpackSize = 0;
};

// LZ4 and Zstd streams made of independent chunks are decoded in parallel
// batches on `backend` (GPU or CPU thread pool); everything else, and
// streams with dependent blocks, use a sequential streaming decoder.
std::unique_ptr<StreamDecoder> createStreamDecoder(const DecoderSpec &spec, Backend *backend, std::size_t batchBytes);

// x86 BCJ filter applied on top of a decoded stream (7z "BCJ" coder).
std::unique_ptr<StreamDecoder> createBcjDecoder(std::unique_ptr<StreamDecoder> inner);

bool canDecode(Method method);

} // namespace vyra
