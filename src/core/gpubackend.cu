// nvCOMP batch backend. All three codecs (Deflate, LZ4, Zstd) share the same
// low-level batched API shape, so one templated driver serves them all.
#include "core/backend.h"

#include <cuda_runtime.h>
#include <nvcomp/deflate.h>
#include <nvcomp/lz4.h>
#include <nvcomp/version.h>
#include <nvcomp/zstd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#if NVCOMP_VER_MAJOR < 5
#error Vyra requires nvCOMP 5.x
#endif

namespace vyra {
namespace {

void check(cudaError_t code, const char *what)
{
    if (code != cudaSuccess) throw Error(std::string("CUDA ") + what + ": " + cudaGetErrorString(code));
}

void checkNv(nvcompStatus_t code, const char *what)
{
    if (code != nvcompSuccess) throw Error(std::string("nvCOMP ") + what + " failed (status " + std::to_string(int(code)) + ")");
}

std::size_t alignUp(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

template<class T> class DeviceArray {
public:
    DeviceArray() = default;
    DeviceArray(const DeviceArray &) = delete;
    DeviceArray &operator=(const DeviceArray &) = delete;
    ~DeviceArray() { if (m_ptr) cudaFree(m_ptr); }
    T *get() const { return m_ptr; }
    void reserve(std::size_t count)
    {
        if (count <= m_capacity) return;
        // Grow geometrically so a sequence of slightly larger batches does
        // not reallocate every time.
        const std::size_t want = std::max(count, m_capacity + m_capacity / 2);
        if (m_ptr) cudaFree(m_ptr);
        m_ptr = nullptr;
        m_capacity = 0;
        check(cudaMalloc(reinterpret_cast<void **>(&m_ptr), std::max<std::size_t>(want, 1) * sizeof(T)), "cudaMalloc");
        m_capacity = want;
    }

private:
    T *m_ptr = nullptr;
    std::size_t m_capacity = 0;
};

struct DeflateCodec {
    using COpts = nvcompBatchedDeflateCompressOpts_t;
    using DOpts = nvcompBatchedDeflateDecompressOpts_t;
    static constexpr std::size_t maxChunk = 65536;
    // Vyra level -> nvCOMP algorithm. Algorithm 0 is entropy-only (poor
    // ratio, not faster in practice), so level 0 maps to 1 as well.
    static COpts copts(int level)
    {
        static const int algorithm[] = {1, 1, 2, 2, 4, 5};
        COpts o{};
        o.algorithm = algorithm[std::clamp(level, 0, 5)];
        return o;
    }
    static DOpts dopts() { return nvcompBatchedDeflateDecompressDefaultOpts; }
    static nvcompStatus_t calign(COpts o, nvcompAlignmentRequirements_t *a) { return nvcompBatchedDeflateCompressGetRequiredAlignments(o, a); }
    static nvcompStatus_t dalign(DOpts o, nvcompAlignmentRequirements_t *a) { return nvcompBatchedDeflateDecompressGetRequiredAlignments(o, a); }
    static nvcompStatus_t ctemp(std::size_t n, std::size_t m, COpts o, std::size_t *t, std::size_t total) { return nvcompBatchedDeflateCompressGetTempSizeAsync(n, m, o, t, total); }
    static nvcompStatus_t cbound(std::size_t m, COpts o, std::size_t *b) { return nvcompBatchedDeflateCompressGetMaxOutputChunkSize(m, o, b); }
    static nvcompStatus_t compress(const void *const *ip, const std::size_t *is, std::size_t m, std::size_t n, void *t, std::size_t tb,
                                   void *const *op, std::size_t *os, COpts o, nvcompStatus_t *st, cudaStream_t s)
    { return nvcompBatchedDeflateCompressAsync(ip, is, m, n, t, tb, op, os, o, st, s); }
    static nvcompStatus_t dtemp(std::size_t n, std::size_t m, DOpts o, std::size_t *t, std::size_t total) { return nvcompBatchedDeflateDecompressGetTempSizeAsync(n, m, o, t, total); }
    static nvcompStatus_t decompress(const void *const *ip, const std::size_t *is, const std::size_t *cap, std::size_t *actual, std::size_t n,
                                     void *t, std::size_t tb, void *const *op, DOpts o, nvcompStatus_t *st, cudaStream_t s)
    { return nvcompBatchedDeflateDecompressAsync(ip, is, cap, actual, n, t, tb, op, o, st, s); }
};

struct Lz4Codec {
    using COpts = nvcompBatchedLZ4CompressOpts_t;
    using DOpts = nvcompBatchedLZ4DecompressOpts_t;
    static constexpr std::size_t maxChunk = std::size_t(1) << 24;
    static COpts copts(int) { return nvcompBatchedLZ4CompressDefaultOpts; }
    static DOpts dopts() { return nvcompBatchedLZ4DecompressDefaultOpts; }
    static nvcompStatus_t calign(COpts o, nvcompAlignmentRequirements_t *a) { return nvcompBatchedLZ4CompressGetRequiredAlignments(o, a); }
    static nvcompStatus_t dalign(DOpts o, nvcompAlignmentRequirements_t *a) { return nvcompBatchedLZ4DecompressGetRequiredAlignments(o, a); }
    static nvcompStatus_t ctemp(std::size_t n, std::size_t m, COpts o, std::size_t *t, std::size_t total) { return nvcompBatchedLZ4CompressGetTempSizeAsync(n, m, o, t, total); }
    static nvcompStatus_t cbound(std::size_t m, COpts o, std::size_t *b) { return nvcompBatchedLZ4CompressGetMaxOutputChunkSize(m, o, b); }
    static nvcompStatus_t compress(const void *const *ip, const std::size_t *is, std::size_t m, std::size_t n, void *t, std::size_t tb,
                                   void *const *op, std::size_t *os, COpts o, nvcompStatus_t *st, cudaStream_t s)
    { return nvcompBatchedLZ4CompressAsync(ip, is, m, n, t, tb, op, os, o, st, s); }
    static nvcompStatus_t dtemp(std::size_t n, std::size_t m, DOpts o, std::size_t *t, std::size_t total) { return nvcompBatchedLZ4DecompressGetTempSizeAsync(n, m, o, t, total); }
    static nvcompStatus_t decompress(const void *const *ip, const std::size_t *is, const std::size_t *cap, std::size_t *actual, std::size_t n,
                                     void *t, std::size_t tb, void *const *op, DOpts o, nvcompStatus_t *st, cudaStream_t s)
    { return nvcompBatchedLZ4DecompressAsync(ip, is, cap, actual, n, t, tb, op, o, st, s); }
};

struct ZstdCodec {
    using COpts = nvcompBatchedZstdCompressOpts_t;
    using DOpts = nvcompBatchedZstdDecompressOpts_t;
    static constexpr std::size_t maxChunk = std::size_t(1) << 24;
    static COpts copts(int) { return nvcompBatchedZstdCompressDefaultOpts; }
    static DOpts dopts() { return nvcompBatchedZstdDecompressDefaultOpts; }
    static nvcompStatus_t calign(COpts o, nvcompAlignmentRequirements_t *a) { return nvcompBatchedZstdCompressGetRequiredAlignments(o, a); }
    static nvcompStatus_t dalign(DOpts o, nvcompAlignmentRequirements_t *a) { return nvcompBatchedZstdDecompressGetRequiredAlignments(o, a); }
    static nvcompStatus_t ctemp(std::size_t n, std::size_t m, COpts o, std::size_t *t, std::size_t total) { return nvcompBatchedZstdCompressGetTempSizeAsync(n, m, o, t, total); }
    static nvcompStatus_t cbound(std::size_t m, COpts o, std::size_t *b) { return nvcompBatchedZstdCompressGetMaxOutputChunkSize(m, o, b); }
    static nvcompStatus_t compress(const void *const *ip, const std::size_t *is, std::size_t m, std::size_t n, void *t, std::size_t tb,
                                   void *const *op, std::size_t *os, COpts o, nvcompStatus_t *st, cudaStream_t s)
    { return nvcompBatchedZstdCompressAsync(ip, is, m, n, t, tb, op, os, o, st, s); }
    static nvcompStatus_t dtemp(std::size_t n, std::size_t m, DOpts o, std::size_t *t, std::size_t total) { return nvcompBatchedZstdDecompressGetTempSizeAsync(n, m, o, t, total); }
    static nvcompStatus_t decompress(const void *const *ip, const std::size_t *is, const std::size_t *cap, std::size_t *actual, std::size_t n,
                                     void *t, std::size_t tb, void *const *op, DOpts o, nvcompStatus_t *st, cudaStream_t s)
    { return nvcompBatchedZstdDecompressAsync(ip, is, cap, actual, n, t, tb, op, o, st, s); }
};

__global__ void packChunks(const std::uint8_t *src, const std::size_t *srcOffsets, const std::size_t *sizes,
                           const std::size_t *dstOffsets, std::size_t count, std::uint8_t *dst)
{
    for (std::size_t i = blockIdx.x; i < count; i += gridDim.x) {
        const std::uint8_t *s = src + srcOffsets[i];
        std::uint8_t *d = dst + dstOffsets[i];
        const std::size_t n = sizes[i];
        for (std::size_t j = threadIdx.x; j < n; j += blockDim.x) d[j] = s[j];
    }
}

class GpuBackend final : public Backend {
public:
    explicit GpuBackend(int device) : m_device(device)
    {
        check(cudaSetDevice(device), "cudaSetDevice");
        cudaDeviceProp prop{};
        check(cudaGetDeviceProperties(&prop, device), "cudaGetDeviceProperties");
        if (prop.major < 7) throw Error("nvCOMP needs compute capability 7.0 or newer");
        m_name = prop.name;
        m_memory = prop.totalGlobalMem;
        check(cudaStreamCreateWithFlags(&m_stream, cudaStreamNonBlocking), "cudaStreamCreate");
        m_profile = std::getenv("VYRA_PROFILE") != nullptr;
        for (auto &e : m_events) check(cudaEventCreate(&e), "cudaEventCreate");
    }
    ~GpuBackend() override
    {
        cudaSetDevice(m_device);
        if (m_profile && m_profileCalls)
            std::fprintf(stderr, "[gpu] %d calls: upload %.2fs compress %.2fs pack+download %.2fs host-sync %.2fs\n", m_profileCalls,
                         m_profileMs[0] / 1000, m_profileMs[1] / 1000, m_profileMs[2] / 1000, m_profileMs[3] / 1000);
        for (auto &e : m_events) if (e) cudaEventDestroy(e);
        // Release pinned staging while m_pinned is still alive.
        m_staging.release();
        if (m_stream) {
            cudaStreamSynchronize(m_stream);
            cudaStreamDestroy(m_stream);
        }
    }

    BackendInfo info() const override
    {
        BackendInfo i;
        i.name = m_name + " (nvCOMP " + std::to_string(NVCOMP_VER_MAJOR) + "." + std::to_string(NVCOMP_VER_MINOR) + "." +
                 std::to_string(NVCOMP_VER_PATCH) + ")";
        i.gpu = true;
        i.deviceMemory = m_memory;
        return i;
    }
    bool canCompress(Method m) const override { return m == Method::Deflate || m == Method::LZ4 || m == Method::Zstd; }
    bool canDecompress(Method m) const override { return canCompress(m); }
    DeflateShape deflateShape() const override { return DeflateShape::FinalBlock; }

    void *allocateHost(std::size_t bytes) override
    {
        cudaSetDevice(m_device);
        void *p = nullptr;
        if (cudaMallocHost(&p, std::max<std::size_t>(bytes, 1)) != cudaSuccess) {
            cudaGetLastError();
            return Backend::allocateHost(bytes); // pinned memory exhausted: pageable still works
        }
        std::lock_guard<std::mutex> lock(m_pinnedMutex);
        m_pinned.push_back(p);
        return p;
    }
    void freeHost(void *p) override
    {
        {
            std::lock_guard<std::mutex> lock(m_pinnedMutex);
            auto it = std::find(m_pinned.begin(), m_pinned.end(), p);
            if (it != m_pinned.end()) {
                m_pinned.erase(it);
                cudaFreeHost(p);
                return;
            }
        }
        Backend::freeHost(p);
    }

    void compress(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks,
                  ChunkOutput &output) override
    {
        switch (method) {
        case Method::Deflate: return compressWith<DeflateCodec>(level, input, chunks, output);
        case Method::LZ4: return compressWith<Lz4Codec>(level, input, chunks, output);
        case Method::Zstd: return compressWith<ZstdCodec>(level, input, chunks, output);
        default: throw Error(std::string("GPU backend cannot compress ") + methodName(method));
        }
    }

    void decompress(Method method, const std::uint8_t *input, const std::vector<Span> &chunks, std::uint8_t *output,
                    const std::vector<Span> &outputs, std::vector<std::size_t> &actualSizes) override
    {
        switch (method) {
        case Method::Deflate: return decompressWith<DeflateCodec>(input, chunks, output, outputs, actualSizes);
        case Method::LZ4: return decompressWith<Lz4Codec>(input, chunks, output, outputs, actualSizes);
        case Method::Zstd: return decompressWith<ZstdCodec>(input, chunks, output, outputs, actualSizes);
        default: throw Error(std::string("GPU backend cannot decompress ") + methodName(method));
        }
    }

private:
    // Uploads `chunks` of `input` as one contiguous transfer when they are
    // already laid out with the required alignment, otherwise repacks them.
    std::uint8_t *stageInput(const std::uint8_t *input, const std::vector<Span> &chunks, std::size_t alignment,
                             std::vector<std::size_t> &deviceOffsets)
    {
        const std::size_t n = chunks.size();
        deviceOffsets.resize(n);
        std::size_t lo = chunks[0].offset, hi = 0;
        bool aligned = true;
        for (const Span &c : chunks) {
            lo = std::min(lo, c.offset);
            hi = std::max(hi, c.offset + c.size);
        }
        for (std::size_t i = 0; i < n; ++i) {
            deviceOffsets[i] = chunks[i].offset - lo;
            if (deviceOffsets[i] % alignment) aligned = false;
        }
        if (aligned && hi - lo <= 2 * totalSize(chunks) + (1u << 20)) {
            m_input.reserve(hi - lo);
            check(cudaMemcpyAsync(m_input.get(), input + lo, hi - lo, cudaMemcpyHostToDevice, m_stream), "upload");
            return m_input.get();
        }
        // Repack into an aligned host staging area, then upload once.
        std::size_t total = 0;
        for (std::size_t i = 0; i < n; ++i) {
            deviceOffsets[i] = total;
            total = alignUp(total + chunks[i].size, alignment);
        }
        if (m_staging.size() < total) m_staging.reset(*this, total + total / 4);
        for (std::size_t i = 0; i < n; ++i)
            std::memcpy(m_staging.data() + deviceOffsets[i], input + chunks[i].offset, chunks[i].size);
        m_input.reserve(total);
        check(cudaMemcpyAsync(m_input.get(), m_staging.data(), total, cudaMemcpyHostToDevice, m_stream), "upload");
        return m_input.get();
    }

    static std::size_t totalSize(const std::vector<Span> &chunks)
    {
        std::size_t t = 0;
        for (const Span &c : chunks) t += c.size;
        return t;
    }

    template<class Codec>
    void compressWith(int level, const std::uint8_t *input, const std::vector<Span> &chunks, ChunkOutput &output)
    {
        const std::size_t n = chunks.size();
        output.offsets.assign(n + 1, 0);
        output.data.clear();
        if (n == 0) return;
        check(cudaSetDevice(m_device), "cudaSetDevice");
        const auto opts = Codec::copts(level);
        nvcompAlignmentRequirements_t align{};
        checkNv(Codec::calign(opts, &align), "alignment query");
        std::size_t maxChunk = 0, total = 0;
        for (const Span &c : chunks) {
            maxChunk = std::max(maxChunk, c.size);
            total += c.size;
        }
        if (maxChunk > Codec::maxChunk) throw Error("chunk too large for the GPU codec");
        std::size_t bound = 0;
        checkNv(Codec::cbound(maxChunk, opts, &bound), "output bound");
        const std::size_t stride = alignUp(bound, std::max<std::size_t>(align.output, 256));
        std::size_t tempBytes = 0;
        checkNv(Codec::ctemp(n, maxChunk, opts, &tempBytes, total), "temp size");

        std::vector<std::size_t> inOffsets;
        const auto hostStart = std::chrono::steady_clock::now();
        if (m_profile) cudaEventRecord(m_events[0], m_stream);
        std::uint8_t *dIn = stageInput(input, chunks, std::max<std::size_t>(align.input, 1), inOffsets);
        if (m_profile) cudaEventRecord(m_events[1], m_stream);
        m_output.reserve(n * stride);
        m_packed.reserve(n * stride);
        m_temp.reserve(tempBytes);
        m_ptrs.reserve(2 * n);
        m_sizes.reserve(4 * n);
        m_statuses.reserve(n);

        // Host-built pointer/size tables, uploaded in one shot.
        m_hostPtrs.resize(2 * n);
        m_hostSizes.resize(4 * n);
        for (std::size_t i = 0; i < n; ++i) {
            m_hostPtrs[i] = dIn + inOffsets[i];
            m_hostPtrs[n + i] = m_output.get() + i * stride;
            m_hostSizes[i] = chunks[i].size;
        }
        void **dPtrs = m_ptrs.get();
        std::size_t *dSizes = m_sizes.get();
        check(cudaMemcpyAsync(dPtrs, m_hostPtrs.data(), 2 * n * sizeof(void *), cudaMemcpyHostToDevice, m_stream), "upload tables");
        check(cudaMemcpyAsync(dSizes, m_hostSizes.data(), n * sizeof(std::size_t), cudaMemcpyHostToDevice, m_stream), "upload tables");
        check(cudaMemsetAsync(m_statuses.get(), 0xFF, n * sizeof(nvcompStatus_t), m_stream), "memset");
        checkNv(Codec::compress(reinterpret_cast<const void *const *>(dPtrs), dSizes, maxChunk, n, m_temp.get(), tempBytes,
                                reinterpret_cast<void *const *>(dPtrs + n), dSizes + n, opts, m_statuses.get(), m_stream),
                "compress");
        if (m_profile) cudaEventRecord(m_events[2], m_stream);
        m_hostStatuses.resize(n);
        check(cudaMemcpyAsync(m_hostSizes.data() + n, dSizes + n, n * sizeof(std::size_t), cudaMemcpyDeviceToHost, m_stream), "download sizes");
        check(cudaMemcpyAsync(m_hostStatuses.data(), m_statuses.get(), n * sizeof(nvcompStatus_t), cudaMemcpyDeviceToHost, m_stream), "download statuses");
        check(cudaStreamSynchronize(m_stream), "compress");
        for (std::size_t i = 0; i < n; ++i) {
            if (m_hostStatuses[i] != nvcompSuccess || m_hostSizes[n + i] > bound)
                throw Error("nvCOMP compression failed for a chunk (status " + std::to_string(int(m_hostStatuses[i])) + ")");
            output.offsets[i + 1] = output.offsets[i] + m_hostSizes[n + i];
        }
        // Pack on the GPU so only the compressed bytes cross PCIe, in one copy.
        const std::size_t packedBytes = output.offsets[n];
        for (std::size_t i = 0; i < n; ++i) {
            m_hostSizes[2 * n + i] = i * stride;           // source offsets
            m_hostSizes[3 * n + i] = output.offsets[i];    // destination offsets
        }
        check(cudaMemcpyAsync(dSizes + 2 * n, m_hostSizes.data() + 2 * n, 2 * n * sizeof(std::size_t), cudaMemcpyHostToDevice, m_stream), "upload offsets");
        const unsigned grid = unsigned(std::min<std::size_t>(n, 65535));
        packChunks<<<grid, 256, 0, m_stream>>>(m_output.get(), dSizes + 2 * n, dSizes + n, dSizes + 3 * n, n, m_packed.get());
        check(cudaGetLastError(), "pack kernel");
        output.data.resize(packedBytes); // pinned when the caller set this backend as allocator
        check(cudaMemcpyAsync(output.data.data(), m_packed.get(), packedBytes, cudaMemcpyDeviceToHost, m_stream), "download");
        if (m_profile) cudaEventRecord(m_events[3], m_stream);
        check(cudaStreamSynchronize(m_stream), "download");
        if (m_profile) {
            float ms[3] = {};
            cudaEventElapsedTime(&ms[0], m_events[0], m_events[1]);
            cudaEventElapsedTime(&ms[1], m_events[1], m_events[2]);
            cudaEventElapsedTime(&ms[2], m_events[2], m_events[3]);
            const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hostStart).count();
            for (int k = 0; k < 3; ++k) m_profileMs[k] += ms[k];
            m_profileMs[3] += total - ms[0] - ms[1] - ms[2];
            ++m_profileCalls;
        }
    }

    template<class Codec>
    void decompressWith(const std::uint8_t *input, const std::vector<Span> &chunks, std::uint8_t *output,
                        const std::vector<Span> &outputs, std::vector<std::size_t> &actualSizes)
    {
        const std::size_t n = chunks.size();
        actualSizes.assign(n, 0);
        if (n == 0) return;
        check(cudaSetDevice(m_device), "cudaSetDevice");
        const auto opts = Codec::dopts();
        nvcompAlignmentRequirements_t align{};
        checkNv(Codec::dalign(opts, &align), "alignment query");
        std::size_t maxOut = 0, totalOut = 0;
        for (const Span &o : outputs) {
            maxOut = std::max(maxOut, o.size);
            totalOut += o.size;
        }
        std::size_t tempBytes = 0;
        checkNv(Codec::dtemp(n, maxOut, opts, &tempBytes, totalOut), "temp size");

        std::vector<std::size_t> inOffsets;
        std::uint8_t *dIn = stageInput(input, chunks, std::max<std::size_t>(align.input, 1), inOffsets);
        // Output slots aligned on the device; contiguous when callers use
        // full-size chunks, which lets the result come back in one copy.
        const std::size_t outAlign = std::max<std::size_t>(align.output, 1);
        std::vector<std::size_t> slot(n);
        std::size_t slotTotal = 0;
        for (std::size_t i = 0; i < n; ++i) {
            slot[i] = slotTotal;
            slotTotal = alignUp(slotTotal + outputs[i].size, outAlign);
        }
        m_output.reserve(slotTotal);
        m_temp.reserve(tempBytes);
        m_ptrs.reserve(2 * n);
        m_sizes.reserve(3 * n);
        m_statuses.reserve(n);
        m_hostPtrs.resize(2 * n);
        m_hostSizes.resize(3 * n);
        for (std::size_t i = 0; i < n; ++i) {
            m_hostPtrs[i] = dIn + inOffsets[i];
            m_hostPtrs[n + i] = m_output.get() + slot[i];
            m_hostSizes[i] = chunks[i].size;
            m_hostSizes[n + i] = outputs[i].size;
        }
        void **dPtrs = m_ptrs.get();
        std::size_t *dSizes = m_sizes.get();
        check(cudaMemcpyAsync(dPtrs, m_hostPtrs.data(), 2 * n * sizeof(void *), cudaMemcpyHostToDevice, m_stream), "upload tables");
        check(cudaMemcpyAsync(dSizes, m_hostSizes.data(), 2 * n * sizeof(std::size_t), cudaMemcpyHostToDevice, m_stream), "upload tables");
        checkNv(Codec::decompress(reinterpret_cast<const void *const *>(dPtrs), dSizes, dSizes + n, dSizes + 2 * n, n,
                                  m_temp.get(), tempBytes, reinterpret_cast<void *const *>(dPtrs + n), opts, m_statuses.get(), m_stream),
                "decompress");
        m_hostStatuses.resize(n);
        check(cudaMemcpyAsync(m_hostSizes.data() + 2 * n, dSizes + 2 * n, n * sizeof(std::size_t), cudaMemcpyDeviceToHost, m_stream), "download sizes");
        check(cudaMemcpyAsync(m_hostStatuses.data(), m_statuses.get(), n * sizeof(nvcompStatus_t), cudaMemcpyDeviceToHost, m_stream), "download statuses");
        bool contiguous = true;
        for (std::size_t i = 0; i < n; ++i)
            if (outputs[i].offset - outputs[0].offset != slot[i]) contiguous = false;
        if (contiguous) {
            check(cudaMemcpyAsync(output + outputs[0].offset, m_output.get(), slot[n - 1] + outputs[n - 1].size,
                                  cudaMemcpyDeviceToHost, m_stream), "download");
        } else {
            for (std::size_t i = 0; i < n; ++i)
                check(cudaMemcpyAsync(output + outputs[i].offset, m_output.get() + slot[i], outputs[i].size,
                                      cudaMemcpyDeviceToHost, m_stream), "download");
        }
        check(cudaStreamSynchronize(m_stream), "decompress");
        for (std::size_t i = 0; i < n; ++i) {
            if (m_hostStatuses[i] != nvcompSuccess || m_hostSizes[2 * n + i] > outputs[i].size)
                throw Error("corrupt data: GPU decompression failed (status " + std::to_string(int(m_hostStatuses[i])) + ")");
            actualSizes[i] = m_hostSizes[2 * n + i];
        }
    }

    int m_device = 0;
    std::string m_name;
    std::size_t m_memory = 0;
    cudaStream_t m_stream = nullptr;
    DeviceArray<std::uint8_t> m_input, m_output, m_packed, m_temp;
    DeviceArray<void *> m_ptrs;
    DeviceArray<std::size_t> m_sizes;
    DeviceArray<nvcompStatus_t> m_statuses;
    std::vector<void *> m_hostPtrs;
    std::vector<std::size_t> m_hostSizes;
    std::vector<nvcompStatus_t> m_hostStatuses;
    HostBuffer m_staging;
    std::mutex m_pinnedMutex;
    std::vector<void *> m_pinned;
    bool m_profile = false;
    cudaEvent_t m_events[4] = {};
    double m_profileMs[4] = {};
    int m_profileCalls = 0;
};

// nvcomp64_5.dll is delay-loaded; probe for it before the first nvCOMP call
// so a missing DLL becomes a clean "unavailable" instead of a crash.
bool nvcompLibraryPresent(std::string *reason)
{
#ifdef _WIN32
    HMODULE module = LoadLibraryW(L"nvcomp64_5.dll");
    if (!module) {
        if (reason) *reason = "nvcomp64_5.dll was not found next to the program";
        return false;
    }
    return true; // keep it loaded; the delay-load helper will reuse it
#else
    (void)reason;
    return true;
#endif
}

} // namespace

std::unique_ptr<Backend> createGpuBackend(int device, std::string *reason)
{
    int count = 0;
    const cudaError_t rc = cudaGetDeviceCount(&count);
    if (rc != cudaSuccess || count == 0) {
        if (reason) *reason = rc == cudaSuccess ? "no NVIDIA GPU found" : std::string("CUDA unavailable: ") + cudaGetErrorString(rc);
        cudaGetLastError();
        return nullptr;
    }
    if (device < 0 || device >= count) {
        if (reason) *reason = "GPU index out of range";
        return nullptr;
    }
    if (!nvcompLibraryPresent(reason)) return nullptr;
    try {
        return std::make_unique<GpuBackend>(device);
    } catch (const std::exception &e) {
        if (reason) *reason = e.what();
        return nullptr;
    }
}

GpuStatus queryGpu()
{
    GpuStatus status;
    int count = 0;
    const cudaError_t rc = cudaGetDeviceCount(&count);
    if (rc != cudaSuccess || count == 0) {
        status.description = rc == cudaSuccess ? "no NVIDIA GPU found" : std::string("CUDA unavailable: ") + cudaGetErrorString(rc);
        cudaGetLastError();
        return status;
    }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) {
        status.description = "cannot query the GPU";
        return status;
    }
    std::string reason;
    if (prop.major < 7) {
        status.description = std::string(prop.name) + " (compute capability 7.0+ required)";
        return status;
    }
    if (!nvcompLibraryPresent(&reason)) {
        status.description = reason;
        return status;
    }
    status.available = true;
    status.description = std::string(prop.name) + " · " + std::to_string(prop.totalGlobalMem >> 20) + " MiB · nvCOMP " +
                         std::to_string(NVCOMP_VER_MAJOR) + "." + std::to_string(NVCOMP_VER_MINOR);
    return status;
}

} // namespace vyra
