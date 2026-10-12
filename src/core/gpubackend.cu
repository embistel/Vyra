// nvCOMP batch backend. All three codecs (Deflate, LZ4, Zstd) share the same
// low-level batched API shape, so one templated driver serves them all.
#include "core/backend.h"
#include "core/framing.h"

#include <cuda_runtime.h>
#include <nvcomp/crc32.h>
#include <nvcomp/deflate.h>
#include <nvcomp/lz4.h>
#include <nvcomp/version.h>
#include <nvcomp/zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

// On Windows (WDDM) cudaSetDevice() waits while another thread sits in
// cudaStreamSynchronize(), which serialised the two codec streams. Select the
// device once per thread instead of on every call.
void useDevice(int device)
{
    thread_local int current = -1;
    if (current == device) return;
    check(cudaSetDevice(device), "cudaSetDevice");
    current = device;
}

// Device/pinned allocations made (VYRA_PROFILE): on WDDM every free
// synchronises the whole device, so steady state must not allocate.
std::atomic<int> g_allocations{0};

template<class T> class DeviceArray {
public:
    DeviceArray() = default;
    DeviceArray(const DeviceArray &) = delete;
    DeviceArray &operator=(const DeviceArray &) = delete;
    ~DeviceArray() { if (m_ptr) cudaFree(m_ptr); }
    T *get() const { return m_ptr; }
    std::size_t bytes() const { return m_capacity * sizeof(T); }
    void release()
    {
        if (m_ptr) cudaFree(m_ptr);
        m_ptr = nullptr;
        m_capacity = 0;
    }
    void reserve(std::size_t count)
    {
        if (count <= m_capacity) return;
        // Grow geometrically so a sequence of slightly larger batches does
        // not reallocate every time.
        const std::size_t want = std::max(count, m_capacity + m_capacity / 2);
        if (m_ptr) cudaFree(m_ptr);
        m_ptr = nullptr;
        m_capacity = 0;
        ++g_allocations;
        check(cudaMalloc(reinterpret_cast<void **>(&m_ptr), std::max<std::size_t>(want, 1) * sizeof(T)), "cudaMalloc");
        m_capacity = want;
    }

private:
    T *m_ptr = nullptr;
    std::size_t m_capacity = 0;
};

struct DeflateCodec {
    static constexpr int id = 0;
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
    static constexpr int id = 1;
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
    static constexpr int id = 2;
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

// ---- Device-side container framing -----------------------------------------
// After nvCOMP has compressed a batch, three small kernels finish the job on
// the GPU so the host only receives final bytes:
//   scanDeflate - finds where each nvCOMP Deflate chunk's final block ends
//   finalize    - per-chunk output sizes, error checks, prefix sum (offsets)
//   pack        - copies chunks to their offsets, adding LZ4 frame headers /
//                 block headers / end marks or Deflate sync-flush joins

enum PackMode : int { kPlain = 0, kLz4Frame = 1, kDeflateJoin = 2 };

namespace dev {

constexpr int kLutBits = 10;
constexpr unsigned long long kScanFailed = ~0ull;

struct Bits {
    const std::uint8_t *data;
    std::size_t size;
    std::size_t pos;
    unsigned long long buf;
    int cnt;
    __device__ void refill()
    {
        while (cnt <= 56) {
            const unsigned long long b = pos < size ? data[pos] : 0;
            buf |= b << cnt;
            ++pos;
            cnt += 8;
        }
    }
    __device__ unsigned peek(int n) const { return unsigned(buf & ((1ull << n) - 1)); }
    __device__ void consume(int n) { buf >>= n; cnt -= n; }
    __device__ unsigned bits(int n)
    {
        if (n == 0) return 0;
        refill();
        const unsigned v = peek(n);
        consume(n);
        return v;
    }
    __device__ std::size_t bitPosition() const { return pos * 8 - std::size_t(cnt); }
    __device__ void seekBit(std::size_t bit)
    {
        pos = bit / 8;
        buf = 0;
        cnt = 0;
        refill();
        consume(int(bit % 8));
    }
    __device__ void alignToByte() { consume(cnt & 7); }
    __device__ bool overrun() const { return bitPosition() > size * 8; }
};

// Same canonical Huffman decoder as deflatestream.cpp (10-bit table, slow
// path for longer codes).
struct Huffman {
    std::uint16_t lut[1 << kLutBits];
    std::uint16_t count[16];
    std::uint16_t symbols[320];

    __device__ bool build(const std::uint8_t *lengths, int n)
    {
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        std::uint16_t offs[17];
        offs[1] = 0;
        for (int len = 1; len < 16; ++len) offs[len + 1] = std::uint16_t(offs[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbols[offs[lengths[s]]++] = std::uint16_t(s);
        for (int i = 0; i < (1 << kLutBits); ++i) lut[i] = 0;
        unsigned code = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            for (int k = 0; k < count[len]; ++k, ++code) {
                const std::uint16_t sym = symbols[index++];
                if (len > kLutBits) continue;
                const unsigned rev = __brev(code) >> (32 - len);
                for (unsigned j = rev; j < (1u << kLutBits); j += 1u << len) lut[j] = std::uint16_t((sym << 4) | len);
            }
            code <<= 1;
        }
        return true;
    }

    __device__ int decode(Bits &br) const
    {
        br.refill();
        const std::uint16_t e = lut[br.peek(kLutBits)];
        if (e) {
            br.consume(e & 15);
            return e >> 4;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= int(br.peek(1));
            br.consume(1);
            const int c = count[len];
            if (code - first < c) return symbols[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

__constant__ std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
__constant__ std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

__device__ bool skipSymbols(Bits &br, const Huffman &lit, const Huffman &dist)
{
    for (;;) {
        const int sym = lit.decode(br);
        if (sym < 0) return false;
        if (sym < 256) continue;
        if (sym == 256) return true;
        if (sym > 285) return false;
        br.bits(kLenExtra[sym - 257]);
        const int d = dist.decode(br);
        if (d < 0 || d >= 30) return false;
        br.bits(kDistExtra[d]);
        if (br.overrun()) return false;
    }
}

// Reads a dynamic block's code lengths (literal/length codes, then distance
// codes). The code-length code is built into `scratch`.
__device__ bool readCodeLengths(Bits &br, Huffman &scratch, std::uint8_t *lengths, int &hlit, int &hdist)
{
    const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    hlit = int(br.bits(5)) + 257;
    hdist = int(br.bits(5)) + 1;
    const int hclen = int(br.bits(4)) + 4;
    if (hlit > 286 || hdist > 30) return false;
    std::uint8_t clen[19] = {};
    for (int i = 0; i < hclen; ++i) clen[order[i]] = std::uint8_t(br.bits(3));
    if (!scratch.build(clen, 19)) return false;
    Huffman &dist = scratch;
    int n = 0;
    while (n < hlit + hdist) {
        const int sym = dist.decode(br);
        if (sym < 0) return false;
        if (sym < 16) { lengths[n++] = std::uint8_t(sym); continue; }
        int repeat = 0;
        std::uint8_t value = 0;
        if (sym == 16) {
            if (n == 0) return false;
            value = lengths[n - 1];
            repeat = 3 + int(br.bits(2));
        } else if (sym == 17) {
            repeat = 3 + int(br.bits(3));
        } else {
            repeat = 11 + int(br.bits(7));
        }
        if (n + repeat > hlit + hdist) return false;
        while (repeat--) lengths[n++] = value;
    }
    return lengths[256] != 0;
}

__device__ bool readDynamicTables(Bits &br, Huffman &lit, Huffman &dist)
{
    std::uint8_t lengths[320];
    int hlit = 0, hdist = 0;
    if (!readCodeLengths(br, dist, lengths, hlit, hdist)) return false;
    return lit.build(lengths, hlit) && dist.build(lengths + hlit, hdist);
}

// Bit-reversed canonical code of `symbol` (as it appears in the LSB-first
// bit stream) and its length.
__device__ unsigned streamCode(const std::uint8_t *lengths, int n, int symbol, int &length)
{
    int count[16] = {};
    for (int s = 0; s < n; ++s) ++count[lengths[s]];
    count[0] = 0;
    length = lengths[symbol];
    unsigned code = 0;
    for (int len = 1; len < length; ++len) code = (code + unsigned(count[len])) << 1;
    for (int s = 0; s < symbol; ++s)
        if (lengths[s] == length) ++code;
    return __brev(code) >> (32 - length);
}

__device__ unsigned bitsAt(const std::uint8_t *data, std::size_t bit, int n)
{
    const std::size_t byte = bit / 8;
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) v |= unsigned(data[byte + k]) << (8 * k); // caller keeps byte + 3 in range
    return (v >> (bit % 8)) & ((1u << n) - 1);
}

__device__ void buildFixed(Huffman &lit, Huffman &dist)
{
    std::uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    lit.build(l, 288);
    for (int i = 0; i < 30; ++i) l[i] = 5;
    dist.build(l, 30);
}

} // namespace dev

// One thread per chunk: walk the Deflate blocks and record the BFINAL bit of
// the final block and the bit just past its end-of-block code.
__global__ void scanDeflateKernel(const std::uint8_t *chunks, std::size_t stride, const std::size_t *compSizes,
                                  const std::uint8_t *streamEnd, std::size_t n, unsigned long long *scan, int verify)
{
    const std::size_t i = std::size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    scan[2 * i] = 0;
    scan[2 * i + 1] = 0;
    if (streamEnd[i]) return; // final chunks stay final, nothing to find
    const std::size_t size = compSizes[i];
    scan[2 * i + 1] = dev::kScanFailed;
    if (size > stride) return;
    const std::uint8_t *data = chunks + i * stride;
    std::size_t fastEnd = 0;
    dev::Bits br{data, size, 0, 0, 0};
    dev::Huffman lit, dist;
    // Fast path. nvCOMP writes exactly ceil(end / 8) bytes (its raw output is
    // what gzip members wrap), so a stream that is one final dynamic block
    // ends inside the last byte: right after the end-of-block code, followed
    // by zero padding. Of the 8 possible end bits the true one always fits
    // that pattern; when exactly one fits, it is the end. Ambiguous or other
    // layouts take the full walk below.
    if (size >= 2 && br.bits(1) == 1 && br.bits(2) == 2) {
        std::uint8_t lengths[320];
        int hlit = 0, hdist = 0;
        if (dev::readCodeLengths(br, dist, lengths, hlit, hdist)) {
            int eobLength = 0;
            const unsigned eob = dev::streamCode(lengths, hlit, 256, eobLength);
            const std::size_t bodyStart = br.bitPosition();
            const std::size_t last = size - 1;
            const unsigned lastByte = data[last];
            int matches = 0;
            std::size_t found = 0;
            for (int r = 1; r <= 8; ++r) {
                const std::size_t end = last * 8 + std::size_t(r);
                if (r < 8 && (lastByte >> r) != 0) continue; // padding must be zero
                if (end < bodyStart + std::size_t(eobLength)) continue;
                const std::size_t from = end - std::size_t(eobLength);
                if (from / 8 + 3 >= stride) continue;
                if (dev::bitsAt(data, from, eobLength) == eob) {
                    ++matches;
                    found = end;
                }
            }
            if (matches == 1) {
                scan[2 * i] = 0;
                scan[2 * i + 1] = found;
                if (!verify) return;
                fastEnd = found; // VYRA_VERIFY_DEFLATE_SCAN: confirm with the full walk
            }
        }
    }
    br = dev::Bits{data, size, 0, 0, 0};
    if (fastEnd) scan[2 * i + 1] = dev::kScanFailed; // until the walk agrees
    for (;;) {
        const std::size_t header = br.bitPosition();
        const bool final = br.bits(1) != 0;
        const unsigned type = br.bits(2);
        if (type == 0) {
            br.alignToByte();
            const unsigned len = br.bits(16);
            const unsigned nlen = br.bits(16);
            if ((len ^ 0xFFFFu) != nlen) return;
            br.seekBit(br.bitPosition() + std::size_t(len) * 8);
        } else if (type == 1) {
            dev::buildFixed(lit, dist);
            if (!dev::skipSymbols(br, lit, dist)) return;
        } else if (type == 2) {
            if (!dev::readDynamicTables(br, lit, dist) || !dev::skipSymbols(br, lit, dist)) return;
        } else {
            return;
        }
        if (br.overrun()) return;
        if (final) {
            scan[2 * i] = header;
            scan[2 * i + 1] = br.bitPosition();
            if (fastEnd && (header != 0 || br.bitPosition() != fastEnd)) scan[2 * i + 1] = dev::kScanFailed;
            return;
        }
    }
}

struct PackResult {
    unsigned long long total;
    unsigned int error; // 0 ok, 1 codec status, 2 Deflate scan, 3 CRC status
    unsigned int chunk;
    unsigned int direct; // 1: packed straight into the host buffer
    unsigned int reserved;
};

struct FinalizeArgs {
    int mode;
    std::size_t n;
    const std::size_t *rawSizes;
    const std::size_t *compSizes;
    const nvcompStatus_t *statuses; // n codec statuses, then crcCount CRC statuses
    std::size_t crcCount;
    std::size_t bound;
    const std::uint8_t *streamStart;
    const std::uint8_t *streamEnd;
    const unsigned long long *scan;
    std::size_t *finalSizes;
    std::size_t *offsets; // n + 1
    PackResult *result;
    std::size_t hostCapacity; // pinned host destination size, 0 if none
};

__device__ std::size_t minSize(std::size_t a, std::size_t b) { return a < b ? a : b; }

__device__ void reportError(PackResult *r, unsigned code, std::size_t chunk)
{
    if (atomicCAS(&r->error, 0u, code) == 0u) r->chunk = unsigned(chunk);
}

__device__ std::size_t finalSize(const FinalizeArgs &a, std::size_t i)
{
    const std::size_t comp = a.compSizes[i];
    if (a.statuses[i] != nvcompSuccess || comp > a.bound) {
        reportError(a.result, 1, i);
        return 0;
    }
    switch (a.mode) {
    case kLz4Frame: {
        const std::size_t raw = a.rawSizes[i];
        return (a.streamStart[i] ? 7 : 0) + 4 + (comp < raw ? comp : raw) + (a.streamEnd[i] ? 4 : 0);
    }
    case kDeflateJoin: {
        if (a.streamEnd[i]) return comp;
        const unsigned long long end = a.scan[2 * i + 1];
        if (end == dev::kScanFailed || (end + 7) / 8 > comp) {
            reportError(a.result, 2, i);
            return 0;
        }
        const unsigned r = unsigned(end % 8);
        return std::size_t((end + 7) / 8) + ((r == 0 || r > 5) ? 5 : 4);
    }
    default:
        return comp;
    }
}

// Single block of 1024 threads: sizes, checks and an exclusive prefix sum.
__global__ void finalizeKernel(FinalizeArgs a)
{
    __shared__ unsigned long long sums[1024];
    const unsigned t = threadIdx.x;
    const std::size_t per = (a.n + 1023) / 1024;
    const std::size_t b = minSize(a.n, std::size_t(t) * per), e = minSize(a.n, b + per);
    unsigned long long local = 0;
    for (std::size_t i = b; i < e; ++i) {
        const std::size_t s = finalSize(a, i);
        a.finalSizes[i] = s;
        local += s;
    }
    for (std::size_t k = t; k < a.crcCount; k += 1024)
        if (a.statuses[a.n + k] != nvcompSuccess) reportError(a.result, 3, k);
    sums[t] = local;
    __syncthreads();
    for (unsigned off = 1; off < 1024; off <<= 1) {
        const unsigned long long v = t >= off ? sums[t - off] : 0;
        __syncthreads();
        sums[t] += v;
        __syncthreads();
    }
    unsigned long long run = t ? sums[t - 1] : 0;
    if (t == 0) a.offsets[0] = 0;
    for (std::size_t i = b; i < e; ++i) {
        run += a.finalSizes[i];
        a.offsets[i + 1] = run;
    }
    if (t == 1023) {
        a.result->total = sums[1023];
        a.result->direct = a.hostCapacity && sums[1023] <= a.hostCapacity ? 1u : 0u;
    }
}

struct Lz4Header {
    std::uint8_t bytes[8];
};

// Block-wide copy that writes 16-byte aligned words (full PCIe write
// transactions when `dst` is host memory) whatever the source alignment.
__device__ void copyBytes(std::uint8_t *dst, const std::uint8_t *src, std::size_t n)
{
    const unsigned t = threadIdx.x, step = blockDim.x;
    const std::size_t head = minSize(n, (16 - (reinterpret_cast<std::uintptr_t>(dst) & 15)) & 15);
    for (std::size_t j = t; j < head; j += step) dst[j] = src[j];
    const std::size_t words = (n - head) / 16;
    uint4 *d = reinterpret_cast<uint4 *>(dst + head);
    const std::uint8_t *s = src + head;
    if ((reinterpret_cast<std::uintptr_t>(s) & 3) == 0) {
        const std::uint32_t *s4 = reinterpret_cast<const std::uint32_t *>(s);
        for (std::size_t k = t; k < words; k += step) d[k] = make_uint4(s4[4 * k], s4[4 * k + 1], s4[4 * k + 2], s4[4 * k + 3]);
    } else {
        for (std::size_t k = t; k < words; k += step) {
            const std::uint8_t *b = s + 16 * k;
            std::uint32_t w[4];
            for (int q = 0; q < 4; ++q)
                w[q] = std::uint32_t(b[4 * q]) | std::uint32_t(b[4 * q + 1]) << 8 | std::uint32_t(b[4 * q + 2]) << 16 | std::uint32_t(b[4 * q + 3]) << 24;
            d[k] = make_uint4(w[0], w[1], w[2], w[3]);
        }
    }
    for (std::size_t j = head + words * 16 + t; j < n; j += step) dst[j] = src[j];
}

struct PackArgs {
    int mode;
    std::size_t n;
    const std::uint8_t *chunks;
    std::size_t stride;
    const std::size_t *compSizes;
    const std::size_t *rawSizes;
    const void *const *inputs;
    const std::uint8_t *streamStart;
    const std::uint8_t *streamEnd;
    const unsigned long long *scan;
    const std::size_t *offsets;
    const PackResult *result;
    Lz4Header header;
    std::uint8_t *dst;     // device buffer (downloaded afterwards)
    std::uint8_t *hostDst; // pinned host buffer, used when result->direct
};

__global__ void packKernel(PackArgs a)
{
    if (a.result->error) return;
    const unsigned t = threadIdx.x;
    std::uint8_t *base = a.result->direct ? a.hostDst : a.dst;
    for (std::size_t i = blockIdx.x; i < a.n; i += gridDim.x) {
        std::uint8_t *d = base + a.offsets[i];
        const std::uint8_t *s = a.chunks + i * a.stride;
        const std::size_t comp = a.compSizes[i];
        if (a.mode == kLz4Frame) {
            std::size_t pos = 0;
            if (a.streamStart[i]) {
                if (t < 7) d[t] = a.header.bytes[t];
                pos = 7;
            }
            const std::size_t raw = a.rawSizes[i];
            const bool stored = comp >= raw;
            const std::size_t len = stored ? raw : comp;
            const std::uint32_t word = std::uint32_t(len) | (stored ? 0x80000000u : 0u);
            if (t < 4) d[pos + t] = std::uint8_t(word >> (8 * t));
            copyBytes(d + pos + 4, stored ? static_cast<const std::uint8_t *>(a.inputs[i]) : s, len);
            if (a.streamEnd[i] && t < 4) d[pos + 4 + len + t] = 0;
        } else if (a.mode == kDeflateJoin && !a.streamEnd[i]) {
            const unsigned long long header = a.scan[2 * i], end = a.scan[2 * i + 1];
            const std::size_t bytes = std::size_t((end + 7) / 8);
            copyBytes(d, s, bytes);
            __syncthreads();
            if (t == 0) {
                d[header / 8] &= std::uint8_t(~(1u << (header % 8))); // clear BFINAL
                const unsigned r = unsigned(end % 8);
                if (r) d[bytes - 1] &= std::uint8_t((1u << r) - 1); // zero the padding bits
                std::uint8_t *tail = d + bytes;
                // Empty stored block (sync-flush marker), see deflatestream.cpp.
                if (r == 0 || r > 5) *tail++ = 0x00;
                tail[0] = 0x00; tail[1] = 0x00; tail[2] = 0xFF; tail[3] = 0xFF;
            }
        } else {
            copyBytes(d, s, comp);
        }
    }
}

// Page-locked host array (cudaMallocHost), grown on demand.
class PinnedBlock {
public:
    PinnedBlock() = default;
    PinnedBlock(const PinnedBlock &) = delete;
    PinnedBlock &operator=(const PinnedBlock &) = delete;
    ~PinnedBlock() { if (m_ptr) cudaFreeHost(m_ptr); }
    std::uint8_t *get() const { return m_ptr; }
    void reserve(std::size_t bytes)
    {
        if (bytes <= m_capacity) return;
        const std::size_t want = std::max(bytes, m_capacity + m_capacity / 2);
        if (m_ptr) cudaFreeHost(m_ptr);
        m_ptr = nullptr;
        m_capacity = 0;
        ++g_allocations;
        check(cudaMallocHost(reinterpret_cast<void **>(&m_ptr), want), "cudaMallocHost");
        m_capacity = want;
    }

private:
    std::uint8_t *m_ptr = nullptr;
    std::size_t m_capacity = 0;
};

// Byte layout of the per-call metadata block (device and pinned host mirror).
struct MetaLayout {
    std::size_t size = 0;
    std::size_t add(std::size_t bytes)
    {
        const std::size_t offset = alignUp(size, 256);
        size = offset + bytes;
        return offset;
    }
};

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
        for (cudaStream_t *s : {&m_upload, &m_compute, &m_download})
            check(cudaStreamCreateWithFlags(s, cudaStreamNonBlocking), "cudaStreamCreate");
        // Up to three batches in flight let upload, compression and download
        // of different batches overlap. How many a job uses depends on its
        // device memory needs (prepareCompress).
        // Debug/test mode: every fast end-of-block result is re-checked by the
        // full Huffman walk; a disagreement fails the job.
        m_verifyScan = std::getenv("VYRA_VERIFY_DEFLATE_SCAN") != nullptr;
        // Zero-copy packing into the caller's pinned output (see compressWith).
        m_unified = prop.unifiedAddressing && prop.canMapHostMemory;
        for (int k = 0; k < kMaxSlots; ++k) {
            auto slot = std::make_unique<Slot>();
            slot->index = k;
            check(cudaEventCreateWithFlags(&slot->uploaded, cudaEventBlockingSync | cudaEventDisableTiming), "cudaEventCreate");
            check(cudaEventCreateWithFlags(&slot->computed, cudaEventBlockingSync | cudaEventDisableTiming), "cudaEventCreate");
            check(cudaEventCreateWithFlags(&slot->downloaded, cudaEventBlockingSync | cudaEventDisableTiming), "cudaEventCreate");
            m_free.push_back(slot.get());
            m_slots.push_back(std::move(slot));
        }
        m_profile = std::getenv("VYRA_PROFILE") != nullptr;
        m_timeline = m_profile && std::string(std::getenv("VYRA_PROFILE")) == "2";
#ifdef _WIN32
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        const std::size_t ram = GlobalMemoryStatusEx(&status) ? std::size_t(status.ullTotalPhys) : (std::size_t(8) << 30);
#else
        const std::size_t ram = std::size_t(8) << 30;
#endif
        m_cacheLimit = std::min(ram / 8, std::size_t(4) << 30);
    }
    ~GpuBackend() override
    {
        cudaSetDevice(m_device);
        for (cudaStream_t s : {m_upload, m_compute, m_download})
            if (s) cudaStreamSynchronize(s);
        for (auto &slot : m_slots) {
            slot->staging.release(); // pinned via this backend
            for (cudaEvent_t e : {slot->uploaded, slot->computed, slot->downloaded})
                if (e) cudaEventDestroy(e);
        }
        m_slots.clear();
        for (auto &block : m_pinnedCache) cudaFreeHost(block.first);
        for (cudaStream_t s : {m_upload, m_compute, m_download})
            if (s) cudaStreamDestroy(s);
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
    int concurrency() const override { return m_activeSlots; }

    // Pinned blocks are kept for the next job (cudaFreeHost stalls the device
    // and cudaMallocHost of hundreds of MiB is slow), up to a cache limit.
    void *allocateHost(std::size_t bytes) override
    {
        bytes = std::max<std::size_t>(bytes, 1);
        {
            std::lock_guard<std::mutex> lock(m_pinnedMutex);
            auto best = m_pinnedCache.end();
            for (auto it = m_pinnedCache.begin(); it != m_pinnedCache.end(); ++it)
                if (it->second >= bytes && it->second <= bytes + bytes / 2 && (best == m_pinnedCache.end() || it->second < best->second)) best = it;
            if (best != m_pinnedCache.end()) {
                void *p = best->first;
                m_pinned.emplace(p, best->second);
                m_cachedBytes -= best->second;
                m_pinnedCache.erase(best);
                return p;
            }
        }
        useDevice(m_device);
        void *p = nullptr;
        ++g_allocations;
        if (cudaMallocHost(&p, bytes) != cudaSuccess) {
            cudaGetLastError();
            return Backend::allocateHost(bytes); // pinned memory exhausted: pageable still works
        }
        std::lock_guard<std::mutex> lock(m_pinnedMutex);
        m_pinned.emplace(p, bytes);
        return p;
    }
    void freeHost(void *p) override
    {
        {
            std::lock_guard<std::mutex> lock(m_pinnedMutex);
            auto it = m_pinned.find(p);
            if (it != m_pinned.end()) {
                const std::size_t bytes = it->second;
                m_pinned.erase(it);
                if (m_cachedBytes + bytes <= m_cacheLimit) {
                    m_pinnedCache.emplace_back(p, bytes);
                    m_cachedBytes += bytes;
                } else {
                    cudaFreeHost(p);
                }
                return;
            }
        }
        Backend::freeHost(p);
    }

    bool registerHost(void *p, std::size_t bytes) override
    {
        useDevice(m_device);
        if (cudaHostRegister(p, bytes, cudaHostRegisterDefault) != cudaSuccess) {
            cudaGetLastError();
            return false;
        }
        std::lock_guard<std::mutex> lock(m_pinnedMutex);
        m_registered.emplace(p, bytes);
        return true;
    }
    void unregisterHost(void *p) override
    {
        {
            std::lock_guard<std::mutex> lock(m_pinnedMutex);
            if (!m_registered.erase(p)) return;
        }
        useDevice(m_device);
        cudaHostUnregister(p);
    }

    // Reserves every slot's buffers for the worst case of a job so nothing is
    // allocated (and the device never stalls) while batches are in flight.
    std::size_t prepareCompress(Method method, int level, std::size_t chunkSize, std::size_t batchSize) override
    {
        switch (method) {
        case Method::Deflate: return prepareWith<DeflateCodec>(level, chunkSize, batchSize);
        case Method::LZ4: return prepareWith<Lz4Codec>(level, chunkSize, batchSize);
        case Method::Zstd: return prepareWith<ZstdCodec>(level, chunkSize, batchSize);
        default: return 0;
        }
    }

    void reportProfile()
    {
        if (!m_profile) return;
        if (m_profileCalls)
            std::fprintf(stderr, "[gpu] %d calls, %zu slots: submit %.2fs (slot wait %.2fs, nvcomp call %.2fs) wait-compute %.2fs download %.2fs (thread time)\n",
                         m_profileCalls.load(), m_slots.size(), m_profileUs[0] / 1e6, m_profileUs[4] / 1e6, m_profileUs[3] / 1e6,
                         m_profileUs[1] / 1e6, m_profileUs[2] / 1e6);
        std::fprintf(stderr, "[gpu] allocations after the first batch: %d\n", g_allocations.load() - m_allocationsAfterFirst);
        m_profileCalls = 0;
        for (auto &u : m_profileUs) u = 0;
    }

    void compress(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks,
                  ChunkOutput &output) override
    {
        dispatchCompress(method, level, input, chunks, kPlain, CompressExtras{}, output);
    }

    bool compressFramed(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks,
                        const CompressExtras &extras, ChunkOutput &output) override
    {
        if (!canCompress(method) || !extras.streamStart || !extras.streamEnd) return false;
        if (method == Method::LZ4) {
            for (const Span &c : chunks)
                if (c.size > extras.lz4BlockMaximum) return false;
        }
        const int mode = method == Method::LZ4 ? kLz4Frame : method == Method::Deflate ? kDeflateJoin : kPlain;
        dispatchCompress(method, level, input, chunks, mode, extras, output);
        return true;
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
    // Device and host buffers for one batch in flight.
    struct Slot {
        DeviceArray<std::uint8_t> input, output, packed, temp, meta;
        PinnedBlock hostUp, hostDown;
        HostBuffer staging;
        cudaEvent_t uploaded = nullptr, computed = nullptr, downloaded = nullptr;
        int index = 0;
        // decompression tables
        DeviceArray<void *> ptrs;
        DeviceArray<std::size_t> sizes;
        DeviceArray<nvcompStatus_t> statuses;
        std::vector<void *> hostPtrs;
        std::vector<std::size_t> hostSizes;
        std::vector<nvcompStatus_t> hostStatuses;
    };

    // Borrows a free slot. If work for it may still be queued when the lease
    // ends early (an exception), waits for the device before handing it back.
    class Lease {
    public:
        // Any free slot, or (wanted != nullptr) that particular one.
        explicit Lease(GpuBackend &owner, Slot *wanted = nullptr) : m_owner(owner)
        {
            std::unique_lock<std::mutex> lock(owner.m_slotMutex);
            auto pick = [&] {
                if (wanted) return std::find(owner.m_free.begin(), owner.m_free.end(), wanted);
                return std::find_if(owner.m_free.begin(), owner.m_free.end(), [&](Slot *x) { return x->index < owner.m_activeSlots; });
            };
            owner.m_slotFreed.wait(lock, [&] { return pick() != owner.m_free.end(); });
            auto it = pick();
            slot = *it;
            owner.m_free.erase(it);
        }
        ~Lease()
        {
            if (busy) cudaDeviceSynchronize();
            std::lock_guard<std::mutex> lock(m_owner.m_slotMutex);
            m_owner.m_free.push_back(slot);
            m_owner.m_slotFreed.notify_all();
        }
        Slot *slot = nullptr;
        bool busy = false;

    private:
        GpuBackend &m_owner;
    };

    // WDDM queues commands in user mode and submits them in batches; a query
    // pushes what was just enqueued to the GPU now instead of whenever the
    // next synchronisation happens.
    // Whether [p, p + size) lies in page-locked memory from this backend
    // (device-accessible through unified addressing).
    bool isPinned(const void *p, std::size_t size)
    {
        if (!m_unified) return false;
        std::lock_guard<std::mutex> lock(m_pinnedMutex);
        const auto *q = static_cast<const std::uint8_t *>(p);
        for (const auto *map : {&m_pinned, &m_registered}) {
            auto it = map->upper_bound(const_cast<void *>(p));
            if (it == map->begin()) continue;
            --it;
            const auto *begin = static_cast<const std::uint8_t *>(it->first);
            if (q >= begin && q + size <= begin + it->second) return true;
        }
        return false;
    }

    static void flush(cudaStream_t stream)
    {
        const cudaError_t rc = cudaStreamQuery(stream);
        if (rc != cudaSuccess && rc != cudaErrorNotReady) check(rc, "stream query");
    }

    void dispatchCompress(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks, int mode,
                          const CompressExtras &extras, ChunkOutput &output)
    {
        switch (method) {
        case Method::Deflate: return compressWith<DeflateCodec>(level, input, chunks, mode, extras, output);
        case Method::LZ4: return compressWith<Lz4Codec>(level, input, chunks, mode, extras, output);
        case Method::Zstd: return compressWith<ZstdCodec>(level, input, chunks, mode, extras, output);
        default: throw Error(std::string("GPU backend cannot compress ") + methodName(method));
        }
    }

    // Copies the input to the slot's device buffer on `stream`: one transfer
    // when the chunks are laid out with the required alignment (always the
    // case for archive batches), otherwise repacked through host staging.
    // Returns the device base and sets deviceOffsets; `hostBase` receives the
    // host offset that maps to device offset 0 (or SIZE_MAX when repacked).
    std::uint8_t *stageInput(Slot &s, cudaStream_t stream, const std::uint8_t *input, const std::vector<Span> &chunks,
                             std::size_t alignment, std::vector<std::size_t> &deviceOffsets, std::size_t *hostBase = nullptr,
                             std::size_t *hostLength = nullptr)
    {
        const std::size_t n = chunks.size();
        deviceOffsets.resize(n);
        std::size_t lo = chunks[0].offset, hi = 0, total = 0;
        bool aligned = true;
        for (const Span &c : chunks) {
            lo = std::min(lo, c.offset);
            hi = std::max(hi, c.offset + c.size);
            total += c.size;
        }
        for (std::size_t i = 0; i < n; ++i) {
            deviceOffsets[i] = chunks[i].offset - lo;
            if (deviceOffsets[i] % alignment) aligned = false;
        }
        if (aligned && hi - lo <= 2 * total + (1u << 20)) {
            s.input.reserve(hi - lo);
            check(cudaMemcpyAsync(s.input.get(), input + lo, hi - lo, cudaMemcpyHostToDevice, stream), "upload");
            if (hostBase) *hostBase = lo;
            if (hostLength) *hostLength = hi - lo;
            return s.input.get();
        }
        std::size_t packed = 0;
        for (std::size_t i = 0; i < n; ++i) {
            deviceOffsets[i] = packed;
            packed = alignUp(packed + chunks[i].size, alignment);
        }
        if (s.staging.size() < packed) s.staging.reset(*this, packed + packed / 4);
        for (std::size_t i = 0; i < n; ++i)
            std::memcpy(s.staging.data() + deviceOffsets[i], input + chunks[i].offset, chunks[i].size);
        s.input.reserve(packed);
        check(cudaMemcpyAsync(s.input.get(), s.staging.data(), packed, cudaMemcpyHostToDevice, stream), "upload");
        if (hostBase) *hostBase = SIZE_MAX;
        return s.input.get();
    }

    struct CompressPlan {
        bool ready = false;
        nvcompAlignmentRequirements_t align{};
        std::size_t bound = 0;
        std::size_t tempChunks = 0, tempTotal = 0, tempBytes = 0;
    };
    static std::uint64_t planKey(int codec, int level, std::size_t maxChunk)
    {
        return (std::uint64_t(codec) << 56) | (std::uint64_t(level & 0xFF) << 48) | std::uint64_t(maxChunk);
    }

    // nvCOMP's size/alignment queries can synchronise the device; ask once per
    // configuration (temp space computed for more chunks/bytes stays valid).
    template<class Codec>
    CompressPlan planFor(int level, std::size_t maxChunk, std::size_t n, std::size_t total)
    {
        const auto opts = Codec::copts(level);
        std::lock_guard<std::mutex> lock(m_planMutex);
        CompressPlan &plan = m_plans[planKey(Codec::id, level, maxChunk)];
        if (!plan.ready) {
            checkNv(Codec::calign(opts, &plan.align), "alignment query");
            checkNv(Codec::cbound(maxChunk, opts, &plan.bound), "output bound");
            plan.ready = true;
        }
        if (n > plan.tempChunks || total > plan.tempTotal) {
            plan.tempChunks = std::max(n, plan.tempChunks);
            plan.tempTotal = std::max(total, plan.tempTotal);
            checkNv(Codec::ctemp(plan.tempChunks, maxChunk, opts, &plan.tempBytes, plan.tempTotal), "temp size");
        }
        return plan;
    }

    nvcompBatchedCRC32Opts_t crcOpts(std::size_t count, std::size_t maxBytes)
    {
        std::lock_guard<std::mutex> lock(m_planMutex);
        // Rounded up so similar batches share one configuration.
        std::size_t bucket = 1, largest = 4096;
        while (bucket < count) bucket <<= 1;
        while (largest < maxBytes) largest <<= 1;
        auto key = std::make_pair(bucket, largest);
        auto it = m_crcConf.find(key);
        if (it == m_crcConf.end()) {
            nvcompCRC32KernelConf_t conf{};
            checkNv(nvcompBatchedCRC32GetHeuristicConf(nvcompCRC32IgnoredInputChunkBytes, bucket, &conf, largest, m_compute), "CRC32 configuration");
            it = m_crcConf.emplace(key, conf).first;
        }
        nvcompBatchedCRC32Opts_t opts{};
        opts.spec = nvcompCRC32;
        opts.kernel_conf = it->second;
        return opts;
    }

    template<class Codec>
    std::size_t prepareWith(int level, std::size_t chunkSize, std::size_t batchSize)
    {
        useDevice(m_device);
        chunkSize = std::min(chunkSize, Codec::maxChunk);
        m_preparedChunk = chunkSize;
        const std::size_t n = (batchSize + chunkSize - 1) / chunkSize;
        const CompressPlan plan = planFor<Codec>(level, chunkSize, n, batchSize);
        const std::size_t stride = alignUp(plan.bound, std::max<std::size_t>(plan.align.output, 256));
        const std::size_t ranges = n + batchSize / (std::size_t(4) << 20) + 64;
        const std::size_t meta = 8 * 256 + n * (2 * sizeof(void *) + 4 * sizeof(std::size_t) + 2 + sizeof(nvcompStatus_t) + 16) +
                                 ranges * (sizeof(void *) + sizeof(std::size_t) + sizeof(nvcompStatus_t) + 4) + 4096;
        // As many slots as fit in device memory (memory our slots already
        // hold counts as available: it is reused or replaced).
        const std::size_t perSlot = batchSize + 2 * (n * stride + n * 16 + 64) + plan.tempBytes + meta;
        std::size_t freeBytes = 0, totalBytes = 0;
        check(cudaMemGetInfo(&freeBytes, &totalBytes), "cudaMemGetInfo");
        std::size_t available = freeBytes;
        for (auto &slot : m_slots)
            available += slot->input.bytes() + slot->output.bytes() + slot->packed.bytes() + slot->temp.bytes() + slot->meta.bytes();
        const std::size_t margin = (std::size_t(256) << 20) + totalBytes / 20;
        const std::size_t fit = available > margin ? (available - margin) / perSlot : 0;
        if (fit == 0)
            throw Error("not enough GPU memory for this job (" + std::to_string(perSlot >> 20) + " MiB needed, " +
                        std::to_string(available >> 20) + " MiB available)");
        // Batches in flight hide the WDDM submission/completion latency
        // (several ms per round trip), which matters more for small batches.
        const int wanted = batchSize <= (std::size_t(128) << 20) ? 4 : 3;
        m_activeSlots = int(std::min<std::size_t>(fit, std::size_t(std::min(wanted, kMaxSlots))));
        // One slot at a time, so batches of another job keep running.
        for (auto &owned : m_slots) {
            Lease lease(*this, owned.get());
            Slot *slot = lease.slot;
            if (slot->index >= m_activeSlots) {
                for (auto *a : {&slot->input, &slot->output, &slot->packed, &slot->temp, &slot->meta}) a->release();
                continue;
            }
            slot->input.reserve(batchSize);
            slot->output.reserve(n * stride);
            slot->packed.reserve(n * stride + n * 16 + 64);
            slot->temp.reserve(plan.tempBytes);
            slot->meta.reserve(meta);
            slot->hostUp.reserve(meta);
            slot->hostDown.reserve(meta);
        }
        if (m_profile)
            std::fprintf(stderr, "[gpu] prepare: %d slots of %zu MiB (temp %zu MiB), device free %zu / %zu MiB\n", m_activeSlots.load(),
                         perSlot >> 20, plan.tempBytes >> 20, freeBytes >> 20, totalBytes >> 20);
        // Framed output rarely exceeds the input; callers grow on demand.
        return batchSize + batchSize / 16 + n * 16 + 4096;
    }

    template<class Codec>
    void compressWith(int level, const std::uint8_t *input, const std::vector<Span> &chunks, int mode,
                      const CompressExtras &extras, ChunkOutput &output)
    {
        const std::size_t n = chunks.size();
        output.offsets.assign(n + 1, 0);
        output.sizes.clear();
        output.data.clear();
        if (extras.crcs) extras.crcs->clear();
        if (n == 0) return;
        const auto t0 = std::chrono::steady_clock::now();
        useDevice(m_device);
        const auto opts = Codec::copts(level);
        std::size_t maxChunk = 0, total = 0;
        for (const Span &c : chunks) {
            maxChunk = std::max(maxChunk, c.size);
            total += c.size;
        }
        if (maxChunk > Codec::maxChunk) throw Error("chunk too large for the GPU codec");
        // Plan with the job's chunk size even when this batch only has small
        // chunks (many small files), so the prepared plan and buffers are reused.
        if (maxChunk <= m_preparedChunk && m_preparedChunk <= Codec::maxChunk) maxChunk = m_preparedChunk;
        const CompressPlan plan = planFor<Codec>(level, maxChunk, n, total);
        const std::size_t stride = alignUp(plan.bound, std::max<std::size_t>(plan.align.output, 256));

        const auto tLease = std::chrono::steady_clock::now();
        Lease lease(*this);
        Slot &s = *lease.slot;
        const auto tLeased = std::chrono::steady_clock::now();
        s.output.reserve(n * stride);
        s.packed.reserve(n * stride + n * 16 + 64); // framing adds at most 15 bytes per chunk
        s.temp.reserve(plan.tempBytes);

        // CRC ranges are computed on the device input (only possible when it
        // is a plain copy of the host range).
        const std::vector<Span> *ranges = extras.crcRanges && extras.crcs ? extras.crcRanges : nullptr;
        const std::size_t r = ranges ? ranges->size() : 0;
        std::size_t maxRange = 0;
        if (ranges)
            for (const Span &x : *ranges) maxRange = std::max(maxRange, x.size);

        MetaLayout m;
        const std::size_t oInPtrs = m.add(n * sizeof(void *)), oOutPtrs = m.add(n * sizeof(void *)),
                          oCrcPtrs = m.add(r * sizeof(void *)), oRaw = m.add(n * sizeof(std::size_t)),
                          oCrcSizes = m.add(r * sizeof(std::size_t)), oFlags = m.add(2 * n);
        const std::size_t uploadBytes = m.size;
        const std::size_t oComp = m.add(n * sizeof(std::size_t)), oStatus = m.add((n + r) * sizeof(nvcompStatus_t)),
                          oScan = m.add(2 * n * sizeof(unsigned long long)), oFinal = m.add(n * sizeof(std::size_t));
        const std::size_t oResult = m.add(sizeof(PackResult)), oOffsets = m.add((n + 1) * sizeof(std::size_t)),
                          oCrcs = m.add(r * sizeof(std::uint32_t));
        const std::size_t downloadBytes = m.size - oResult;
        s.meta.reserve(m.size);
        s.hostUp.reserve(uploadBytes);
        s.hostDown.reserve(downloadBytes);
        std::uint8_t *dm = s.meta.get(), *hu = s.hostUp.get();
        auto dp = [&](std::size_t o) { return dm + o; };

        std::vector<std::size_t> inOffsets;
        std::size_t hostBase = SIZE_MAX, hostLength = 0;
        bool crcOnDevice = false;
        {
            std::lock_guard<std::mutex> issue(m_issueMutex);
            lease.busy = true;
            std::uint8_t *dIn = stageInput(s, m_upload, input, chunks, std::max<std::size_t>(plan.align.input, 1), inOffsets, &hostBase, &hostLength);
            crcOnDevice = r > 0 && hostBase != SIZE_MAX;
            if (crcOnDevice) {
                for (const Span &x : *ranges)
                    if (x.offset < hostBase || x.offset + x.size - hostBase > hostLength) crcOnDevice = false;
            }
            const std::size_t rUsed = crcOnDevice ? r : 0;
            auto *inPtrs = reinterpret_cast<void **>(hu + oInPtrs);
            auto *outPtrs = reinterpret_cast<void **>(hu + oOutPtrs);
            auto *crcPtrs = reinterpret_cast<void **>(hu + oCrcPtrs);
            auto *raw = reinterpret_cast<std::size_t *>(hu + oRaw);
            auto *crcSizes = reinterpret_cast<std::size_t *>(hu + oCrcSizes);
            std::uint8_t *flags = hu + oFlags;
            for (std::size_t i = 0; i < n; ++i) {
                inPtrs[i] = dIn + inOffsets[i];
                outPtrs[i] = s.output.get() + i * stride;
                raw[i] = chunks[i].size;
                flags[i] = extras.streamStart ? (*extras.streamStart)[i] : 0;
                flags[n + i] = extras.streamEnd ? (*extras.streamEnd)[i] : 0;
            }
            for (std::size_t k = 0; k < rUsed; ++k) {
                crcPtrs[k] = dIn + ((*ranges)[k].offset - hostBase);
                crcSizes[k] = (*ranges)[k].size;
            }
            check(cudaMemcpyAsync(dm, hu, uploadBytes, cudaMemcpyHostToDevice, m_upload), "upload tables");
            check(cudaEventRecord(s.uploaded, m_upload), "event");
            flush(m_upload);

            check(cudaStreamWaitEvent(m_compute, s.uploaded, 0), "event wait");
            check(cudaMemsetAsync(dp(oResult), 0, sizeof(PackResult), m_compute), "memset");
            auto *dInPtrs = reinterpret_cast<void **>(dp(oInPtrs));
            auto *dRaw = reinterpret_cast<std::size_t *>(dp(oRaw));
            auto *dComp = reinterpret_cast<std::size_t *>(dp(oComp));
            auto *dStatus = reinterpret_cast<nvcompStatus_t *>(dp(oStatus));
            const auto tc0 = std::chrono::steady_clock::now();
            checkNv(Codec::compress(reinterpret_cast<const void *const *>(dInPtrs), dRaw, maxChunk, n, s.temp.get(), plan.tempBytes,
                                    reinterpret_cast<void *const *>(dp(oOutPtrs)), dComp, opts, dStatus, m_compute),
                    "compress");
            if (m_profile) m_profileUs[3] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tc0).count();
            if (rUsed) {
                checkNv(nvcompBatchedCRC32Async(reinterpret_cast<const void *const *>(dp(oCrcPtrs)),
                                                reinterpret_cast<const std::size_t *>(dp(oCrcSizes)), rUsed,
                                                reinterpret_cast<std::uint32_t *>(dp(oCrcs)), crcOpts(rUsed, maxRange),
                                                nvcompCRC32OnlySegment, dStatus + n, m_compute),
                        "CRC32");
            }
            auto *dScan = reinterpret_cast<unsigned long long *>(dp(oScan));
            if (mode == kDeflateJoin) {
                scanDeflateKernel<<<unsigned((n + 63) / 64), 64, 0, m_compute>>>(s.output.get(), stride, dComp, dp(oFlags + n), n, dScan, m_verifyScan ? 1 : 0);
                check(cudaGetLastError(), "scan kernel");
            }
            FinalizeArgs fa{};
            fa.mode = mode;
            fa.n = n;
            fa.rawSizes = dRaw;
            fa.compSizes = dComp;
            fa.statuses = dStatus;
            fa.crcCount = rUsed;
            fa.bound = plan.bound;
            fa.streamStart = dp(oFlags);
            fa.streamEnd = dp(oFlags + n);
            fa.scan = dScan;
            fa.finalSizes = reinterpret_cast<std::size_t *>(dp(oFinal));
            fa.offsets = reinterpret_cast<std::size_t *>(dp(oOffsets));
            fa.result = reinterpret_cast<PackResult *>(dp(oResult));
            // Zero-copy: when the result fits the caller's pinned output
            // buffer, the pack kernel writes it there over PCIe. That saves a
            // host round trip and a copy-engine download per batch (which
            // queues behind other batches' uploads).
            const bool hostTarget = output.data.capacity() > 0 && isPinned(output.data.data(), output.data.capacity());
            fa.hostCapacity = hostTarget ? output.data.capacity() : 0;
            finalizeKernel<<<1, 1024, 0, m_compute>>>(fa);
            check(cudaGetLastError(), "finalize kernel");
            PackArgs pa{};
            pa.mode = mode;
            pa.n = n;
            pa.chunks = s.output.get();
            pa.stride = stride;
            pa.compSizes = dComp;
            pa.rawSizes = dRaw;
            pa.inputs = reinterpret_cast<const void *const *>(dInPtrs);
            pa.streamStart = fa.streamStart;
            pa.streamEnd = fa.streamEnd;
            pa.scan = dScan;
            pa.offsets = fa.offsets;
            pa.result = fa.result;
            if (mode == kLz4Frame) writeLz4FrameHeader(pa.header.bytes, extras.lz4BlockMaximum);
            pa.dst = s.packed.get();
            pa.hostDst = hostTarget ? output.data.data() : nullptr;
            const unsigned packBlocks = unsigned(std::min<std::size_t>(n, 65535));
            if (hostTarget) {
                // Zero-copy pack on the download stream: it writes over PCIe
                // into the caller's pinned buffer while the compute stream
                // already runs the next batch, and no host round trip or
                // copy-engine download (queued behind uploads) is needed.
                check(cudaMemcpyAsync(s.hostDown.get(), dp(oResult), downloadBytes, cudaMemcpyDeviceToHost, m_compute), "download sizes");
                check(cudaEventRecord(s.computed, m_compute), "event");
                check(cudaStreamWaitEvent(m_download, s.computed, 0), "event wait");
                packKernel<<<packBlocks, 256, 0, m_download>>>(pa);
                check(cudaGetLastError(), "pack kernel");
                check(cudaEventRecord(s.downloaded, m_download), "event");
                flush(m_compute);
                flush(m_download);
            } else {
                packKernel<<<packBlocks, 256, 0, m_compute>>>(pa);
                check(cudaGetLastError(), "pack kernel");
                check(cudaMemcpyAsync(s.hostDown.get(), dp(oResult), downloadBytes, cudaMemcpyDeviceToHost, m_compute), "download sizes");
                check(cudaEventRecord(s.computed, m_compute), "event");
                flush(m_compute);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        // The host input is only needed until it is on the device (raw LZ4
        // blocks are copied from the device copy); hand it back early.
        if (extras.inputReleased && (crcOnDevice || r == 0)) {
            check(cudaEventSynchronize(s.uploaded), "upload");
            extras.inputReleased();
        }
        check(cudaEventSynchronize(s.computed), "compress");
        const auto t2 = std::chrono::steady_clock::now();
        const std::uint8_t *hd = s.hostDown.get();
        PackResult result;
        std::memcpy(&result, hd, sizeof(result));
        if (result.error) {
            static const char *what[] = {"", "nvCOMP compression failed for a chunk", "GPU produced a Deflate chunk that could not be parsed",
                                         "GPU CRC32 failed"};
            throw Error(std::string(what[std::min(result.error, 3u)]) + " (chunk " + std::to_string(result.chunk) + ")");
        }
        std::memcpy(output.offsets.data(), hd + (oOffsets - oResult), (n + 1) * sizeof(std::size_t));
        if (output.offsets[n] != result.total) throw Error("internal error: GPU offsets do not add up");
        if (crcOnDevice) {
            extras.crcs->resize(r);
            std::memcpy(extras.crcs->data(), hd + (oCrcs - oResult), r * sizeof(std::uint32_t));
        }
        if (result.direct) {
            check(cudaEventSynchronize(s.downloaded), "pack"); // the zero-copy pack has finished
            output.data.resize(std::size_t(result.total));     // written by the pack kernel; capacity unchanged
        } else {
            output.data.resize(std::size_t(result.total)); // pinned when the caller set this backend as allocator
            std::lock_guard<std::mutex> issue(m_issueMutex);
            check(cudaMemcpyAsync(output.data.data(), s.packed.get(), std::size_t(result.total), cudaMemcpyDeviceToHost, m_download), "download");
            check(cudaEventRecord(s.downloaded, m_download), "event");
            flush(m_download);
        }
        if (!result.direct) check(cudaEventSynchronize(s.downloaded), "download");
        lease.busy = false;
        if (m_profile) {
            const auto t3 = std::chrono::steady_clock::now();
            auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
            m_profileUs[0] += us(t0, t1);
            m_profileUs[1] += us(t1, t2);
            m_profileUs[2] += us(t2, t3);
            m_profileUs[4] += us(tLease, tLeased);
            if (++m_profileCalls == 1) m_allocationsAfterFirst = g_allocations.load();
            if (m_timeline) {
                auto ms = [](std::chrono::steady_clock::time_point t) { return std::fmod(std::chrono::duration<double, std::milli>(t.time_since_epoch()).count(), 1e6); };
                std::fprintf(stderr, "[gpu-tl] slot %p entry %8.1f leased %8.1f submitted %8.1f computed %8.1f downloaded %8.1f  (%zu MiB)\n",
                             static_cast<void *>(&s), ms(t0), ms(tLeased), ms(t1), ms(t2), ms(t3), total >> 20);
            }
        }
    }

    template<class Codec>
    void decompressWith(const std::uint8_t *input, const std::vector<Span> &chunks, std::uint8_t *output,
                        const std::vector<Span> &outputs, std::vector<std::size_t> &actualSizes)
    {
        const std::size_t n = chunks.size();
        actualSizes.assign(n, 0);
        if (n == 0) return;
        useDevice(m_device);
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

        Lease lease(*this);
        Slot &s = *lease.slot;
        lease.busy = true;
        const cudaStream_t stream = m_compute;
        std::vector<std::size_t> inOffsets;
        std::uint8_t *dIn = stageInput(s, stream, input, chunks, std::max<std::size_t>(align.input, 1), inOffsets);
        // Output slots aligned on the device; contiguous when callers use
        // full-size chunks, which lets the result come back in one copy.
        const std::size_t outAlign = std::max<std::size_t>(align.output, 1);
        std::vector<std::size_t> slot(n);
        std::size_t slotTotal = 0;
        for (std::size_t i = 0; i < n; ++i) {
            slot[i] = slotTotal;
            slotTotal = alignUp(slotTotal + outputs[i].size, outAlign);
        }
        s.output.reserve(slotTotal);
        s.temp.reserve(tempBytes);
        s.ptrs.reserve(2 * n);
        s.sizes.reserve(3 * n);
        s.statuses.reserve(n);
        s.hostPtrs.resize(2 * n);
        s.hostSizes.resize(3 * n);
        for (std::size_t i = 0; i < n; ++i) {
            s.hostPtrs[i] = dIn + inOffsets[i];
            s.hostPtrs[n + i] = s.output.get() + slot[i];
            s.hostSizes[i] = chunks[i].size;
            s.hostSizes[n + i] = outputs[i].size;
        }
        void **dPtrs = s.ptrs.get();
        std::size_t *dSizes = s.sizes.get();
        check(cudaMemcpyAsync(dPtrs, s.hostPtrs.data(), 2 * n * sizeof(void *), cudaMemcpyHostToDevice, stream), "upload tables");
        check(cudaMemcpyAsync(dSizes, s.hostSizes.data(), 2 * n * sizeof(std::size_t), cudaMemcpyHostToDevice, stream), "upload tables");
        checkNv(Codec::decompress(reinterpret_cast<const void *const *>(dPtrs), dSizes, dSizes + n, dSizes + 2 * n, n,
                                  s.temp.get(), tempBytes, reinterpret_cast<void *const *>(dPtrs + n), opts, s.statuses.get(), stream),
                "decompress");
        s.hostStatuses.resize(n);
        check(cudaMemcpyAsync(s.hostSizes.data() + 2 * n, dSizes + 2 * n, n * sizeof(std::size_t), cudaMemcpyDeviceToHost, stream), "download sizes");
        check(cudaMemcpyAsync(s.hostStatuses.data(), s.statuses.get(), n * sizeof(nvcompStatus_t), cudaMemcpyDeviceToHost, stream), "download statuses");
        bool contiguous = true;
        for (std::size_t i = 0; i < n; ++i)
            if (outputs[i].offset - outputs[0].offset != slot[i]) contiguous = false;
        if (contiguous) {
            check(cudaMemcpyAsync(output + outputs[0].offset, s.output.get(), slot[n - 1] + outputs[n - 1].size,
                                  cudaMemcpyDeviceToHost, stream), "download");
        } else {
            for (std::size_t i = 0; i < n; ++i)
                check(cudaMemcpyAsync(output + outputs[i].offset, s.output.get() + slot[i], outputs[i].size,
                                      cudaMemcpyDeviceToHost, stream), "download");
        }
        check(cudaEventRecord(s.computed, stream), "event");
        check(cudaEventSynchronize(s.computed), "decompress");
        lease.busy = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (s.hostStatuses[i] != nvcompSuccess || s.hostSizes[2 * n + i] > outputs[i].size)
                throw Error("corrupt data: GPU decompression failed (status " + std::to_string(int(s.hostStatuses[i])) + ")");
            actualSizes[i] = s.hostSizes[2 * n + i];
        }
    }

    int m_device = 0;
    std::string m_name;
    std::size_t m_memory = 0;
    cudaStream_t m_upload = nullptr, m_compute = nullptr, m_download = nullptr;
    std::mutex m_issueMutex; // keeps one call's stream operations together
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<Slot *> m_free;
    std::mutex m_slotMutex;
    std::condition_variable m_slotFreed;
    std::mutex m_pinnedMutex;
    std::map<void *, std::size_t> m_pinned;                     // handed out
    std::map<void *, std::size_t> m_registered;                 // caller memory, page-locked
    std::vector<std::pair<void *, std::size_t>> m_pinnedCache;  // free for reuse
    std::size_t m_cachedBytes = 0;
    std::size_t m_cacheLimit = 0;
    int m_allocationsAfterFirst = 0;
    std::mutex m_planMutex;
    std::map<std::uint64_t, CompressPlan> m_plans;
    std::map<std::pair<std::size_t, std::size_t>, nvcompCRC32KernelConf_t> m_crcConf;
    bool m_profile = false;
    bool m_timeline = false;
    bool m_verifyScan = false;
    bool m_unified = false; // pinned host memory is device-accessible (zero-copy)
    std::size_t m_preparedChunk = 0;
    static constexpr int kMaxSlots = 8;
    std::atomic<int> m_activeSlots{3};
    std::atomic<int> m_profileCalls{0};
    std::atomic<std::int64_t> m_profileUs[5] = {};
};

// One GpuBackend per device lives for the whole process: the CUDA context,
// device buffers and pinned host blocks carry over from one job to the next.
// It is never destroyed (process exit releases everything; CUDA teardown
// order at exit makes explicit destruction pointless).
GpuBackend &sharedGpuBackend(int device)
{
    static std::mutex mutex;
    static std::map<int, GpuBackend *> instances;
    std::lock_guard<std::mutex> lock(mutex);
    GpuBackend *&instance = instances[device];
    if (!instance) instance = new GpuBackend(device);
    return *instance;
}

// What callers own: a handle that forwards to the shared instance.
class SharedGpuBackend final : public Backend {
public:
    explicit SharedGpuBackend(GpuBackend &shared) : m_shared(shared) {}
    ~SharedGpuBackend() override { m_shared.reportProfile(); }
    BackendInfo info() const override { return m_shared.info(); }
    bool canCompress(Method m) const override { return m_shared.canCompress(m); }
    bool canDecompress(Method m) const override { return m_shared.canDecompress(m); }
    DeflateShape deflateShape() const override { return m_shared.deflateShape(); }
    void compress(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks, ChunkOutput &output) override
    {
        m_shared.compress(method, level, input, chunks, output);
    }
    bool compressFramed(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks,
                        const CompressExtras &extras, ChunkOutput &output) override
    {
        return m_shared.compressFramed(method, level, input, chunks, extras, output);
    }
    std::size_t prepareCompress(Method method, int level, std::size_t chunkSize, std::size_t batchSize) override
    {
        return m_shared.prepareCompress(method, level, chunkSize, batchSize);
    }
    int concurrency() const override { return m_shared.concurrency(); }
    void decompress(Method method, const std::uint8_t *input, const std::vector<Span> &chunks, std::uint8_t *output,
                    const std::vector<Span> &outputs, std::vector<std::size_t> &actualSizes) override
    {
        m_shared.decompress(method, input, chunks, output, outputs, actualSizes);
    }
    void *allocateHost(std::size_t bytes) override { return m_shared.allocateHost(bytes); }
    void freeHost(void *pointer) override { m_shared.freeHost(pointer); }
    bool registerHost(void *pointer, std::size_t bytes) override { return m_shared.registerHost(pointer, bytes); }
    void unregisterHost(void *pointer) override { m_shared.unregisterHost(pointer); }

private:
    GpuBackend &m_shared;
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
        return std::make_unique<SharedGpuBackend>(sharedGpuBackend(device));
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
