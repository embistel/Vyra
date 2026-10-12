#include "core/backend.h"
#include "core/crc32.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace vyra {

const char *formatName(Format format)
{
    switch (format) {
    case Format::Zip: return "zip";
    case Format::SevenZip: return "7z";
    case Format::Gzip: return "gz";
    case Format::Lz4: return "lz4";
    }
    return "7z";
}

const char *methodName(Method method)
{
    switch (method) {
    case Method::Store: return "Store";
    case Method::Deflate: return "Deflate";
    case Method::LZ4: return "LZ4";
    case Method::Zstd: return "Zstd";
    case Method::LZMA: return "LZMA";
    case Method::LZMA2: return "LZMA2";
    case Method::BCJ: return "BCJ";
    case Method::Other: break;
    }
    return "Other";
}

void ByteBuffer::assign(const std::uint8_t *p, std::size_t n)
{
    resize(n);
    if (n) std::memcpy(m_data, p, n);
}

void ByteBuffer::swap(ByteBuffer &other) noexcept
{
    std::swap(m_allocator, other.m_allocator);
    std::swap(m_data, other.m_data);
    std::swap(m_size, other.m_size);
    std::swap(m_capacity, other.m_capacity);
}

void ByteBuffer::grow(std::size_t n)
{
    const std::size_t capacity = std::max(n, m_capacity + m_capacity / 4);
    auto *fresh = static_cast<std::uint8_t *>(m_allocator ? m_allocator->allocateHost(capacity) : ::operator new(capacity));
    if (m_size) std::memcpy(fresh, m_data, m_size);
    release();
    m_data = fresh;
    m_capacity = capacity;
}

void ByteBuffer::release()
{
    if (m_data) {
        if (m_allocator) m_allocator->freeHost(m_data);
        else ::operator delete(m_data);
    }
    m_data = nullptr;
    m_capacity = 0;
}

void *Backend::allocateHost(std::size_t bytes) { return ::operator new(bytes ? bytes : 1); }

void Backend::compressChecked(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks,
                              const std::vector<Span> &crcRanges, std::vector<std::uint32_t> &crcs, ChunkOutput &output)
{
    crcs.resize(crcRanges.size());
    for (std::size_t i = 0; i < crcRanges.size(); ++i) crcs[i] = crc32(input + crcRanges[i].offset, crcRanges[i].size);
    compress(method, level, input, chunks, output);
}
void Backend::freeHost(void *pointer) { ::operator delete(pointer); }

void HostBuffer::reset(Backend &backend, std::size_t bytes)
{
    release();
    m_backend = &backend;
    m_data = static_cast<std::uint8_t *>(backend.allocateHost(bytes));
    m_size = bytes;
}

void HostBuffer::release()
{
    if (m_data && m_backend) m_backend->freeHost(m_data);
    m_data = nullptr;
    m_size = 0;
}

void HostBuffer::swap(HostBuffer &other) noexcept
{
    std::swap(m_backend, other.m_backend);
    std::swap(m_data, other.m_data);
    std::swap(m_size, other.m_size);
}

#ifndef VYRA_HAVE_CUDA
std::unique_ptr<Backend> createGpuBackend(int, std::string *reason)
{
    if (reason) *reason = "this build has no CUDA support";
    return nullptr;
}

GpuStatus queryGpu() { return {false, "this build has no CUDA support"}; }
#endif

std::unique_ptr<Backend> selectBackend(BackendKind kind, ThreadPool &pool, int device)
{
    if (kind != BackendKind::Cpu) {
        std::string reason;
        auto gpu = createGpuBackend(device, &reason);
        if (gpu) return gpu;
        if (kind == BackendKind::Gpu) throw Error("GPU backend unavailable: " + reason);
    }
    return createCpuBackend(pool);
}

} // namespace vyra
