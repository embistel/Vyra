#pragma once

#include "core/common.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vyra {

class ThreadPool;

struct Span {
    std::size_t offset = 0;
    std::size_t size = 0;
};

class Backend;

// Byte storage that never zero-fills and keeps its capacity. Batches are
// hundreds of MiB, so value-initialisation and fresh page faults would
// dominate. With an allocator backend the memory is pinned (GPU transfers
// land in it directly).
class ByteBuffer {
public:
    ByteBuffer() = default;
    ~ByteBuffer() { release(); }
    ByteBuffer(const ByteBuffer &) = delete;
    ByteBuffer &operator=(const ByteBuffer &) = delete;
    ByteBuffer(ByteBuffer &&other) noexcept { swap(other); }
    ByteBuffer &operator=(ByteBuffer &&other) noexcept { swap(other); return *this; }

    void setAllocator(Backend *backend) { if (!m_data) m_allocator = backend; }
    std::uint8_t *data() { return m_data; }
    const std::uint8_t *data() const { return m_data; }
    std::size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    void clear() { m_size = 0; }
    void resize(std::size_t n) { if (n > m_capacity) grow(n); m_size = n; }
    void assign(const std::uint8_t *p, std::size_t n);
    void swap(ByteBuffer &other) noexcept;

private:
    void grow(std::size_t n);
    void release();
    Backend *m_allocator = nullptr;
    std::uint8_t *m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
};

// Compressed chunks packed back to back: chunk i is
// data[offsets[i] .. offsets[i + 1]).
struct ChunkOutput {
    ByteBuffer data;
    std::vector<std::size_t> offsets;
    std::size_t count() const { return offsets.empty() ? 0 : offsets.size() - 1; }
    const std::uint8_t *chunk(std::size_t i) const { return data.data() + offsets[i]; }
    std::size_t chunkSize(std::size_t i) const { return offsets[i + 1] - offsets[i]; }
};

// How a backend terminates each raw Deflate chunk.
//   FinalBlock  - each chunk is a complete stream ending in a BFINAL block
//                 (nvCOMP). Needs deflatestream.h to make it joinable.
//   SyncFlushed - each chunk ends with an empty stored block, byte aligned
//                 and non-final (zlib Z_SYNC_FLUSH). The last chunk of a stream
//                 needs a final empty block appended.
enum class DeflateShape { FinalBlock, SyncFlushed };

struct BackendInfo {
    std::string name;     // e.g. "NVIDIA GeForce RTX 3090 (nvCOMP 5.3.0)"
    bool gpu = false;
    std::size_t deviceMemory = 0;
};

// A batch codec: compresses or decompresses many independent chunks at once.
// GPU backends get their speed from large batches; the CPU backend runs the
// chunks on a thread pool. Future Intel/AMD iGPU backends plug in here.
class Backend {
public:
    virtual ~Backend() = default;
    virtual BackendInfo info() const = 0;
    virtual bool canCompress(Method method) const = 0;
    virtual bool canDecompress(Method method) const = 0;
    virtual DeflateShape deflateShape() const = 0;

    // level: method specific (Deflate 0..5 nvCOMP scale, Zstd 1..19).
    virtual void compress(Method method, int level, const std::uint8_t *input,
                          const std::vector<Span> &chunks, ChunkOutput &output) = 0;

    // Decodes independent chunks. outputs[i] gives the destination slice and
    // its capacity inside `output`; actualSizes[i] receives the decoded size.
    // Throws vyra::Error if any chunk is corrupt.
    virtual void decompress(Method method, const std::uint8_t *input, const std::vector<Span> &chunks,
                            std::uint8_t *output, const std::vector<Span> &outputs,
                            std::vector<std::size_t> &actualSizes) = 0;

    // Host staging memory. GPU backends return page-locked memory so
    // transfers run at full PCIe bandwidth.
    virtual void *allocateHost(std::size_t bytes);
    virtual void freeHost(void *pointer);
};

// RAII host buffer allocated through a backend.
class HostBuffer {
public:
    HostBuffer() = default;
    HostBuffer(Backend &backend, std::size_t bytes) { reset(backend, bytes); }
    ~HostBuffer() { release(); }
    HostBuffer(const HostBuffer &) = delete;
    HostBuffer &operator=(const HostBuffer &) = delete;
    HostBuffer(HostBuffer &&other) noexcept { swap(other); }
    HostBuffer &operator=(HostBuffer &&other) noexcept { release(); swap(other); return *this; }

    void reset(Backend &backend, std::size_t bytes);
    void release();
    std::uint8_t *data() const { return m_data; }
    std::size_t size() const { return m_size; }

private:
    void swap(HostBuffer &other) noexcept;
    Backend *m_backend = nullptr;
    std::uint8_t *m_data = nullptr;
    std::size_t m_size = 0;
};

std::unique_ptr<Backend> createCpuBackend(ThreadPool &pool);

// Returns nullptr (and a reason) when no usable NVIDIA GPU / nvCOMP exists.
std::unique_ptr<Backend> createGpuBackend(int device, std::string *reason = nullptr);

struct GpuStatus {
    bool available = false;
    std::string description; // device name or the reason it is unavailable
};
GpuStatus queryGpu();

// Picks a backend for the request. Auto prefers the GPU and falls back to CPU.
std::unique_ptr<Backend> selectBackend(BackendKind kind, ThreadPool &pool, int device = 0);

} // namespace vyra
