#include "core/backend.h"
#include "core/threadpool.h"

#include <lz4.h>
#include <lz4hc.h>
#include <zlib-ng.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>

namespace vyra {
namespace {

int zlibLevel(int level)
{
    static const int map[] = {1, 1, 2, 4, 6, 9};
    return map[std::clamp(level, 0, 5)];
}

int zstdLevel(int level)
{
    static const int map[] = {1, 1, 3, 6, 12, 19};
    return map[std::clamp(level, 0, 5)];
}

struct DeflateContext {
    zng_stream stream{};
    int level = -100;
    bool ready = false;
    ~DeflateContext() { if (ready) zng_deflateEnd(&stream); }
    void prepare(int zlevel)
    {
        if (ready && level == zlevel) { zng_deflateReset(&stream); return; }
        if (ready) zng_deflateEnd(&stream);
        std::memset(&stream, 0, sizeof(stream));
        if (zng_deflateInit2(&stream, zlevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw Error("zlib-ng deflate initialisation failed");
        ready = true;
        level = zlevel;
    }
};

struct InflateContext {
    zng_stream stream{};
    bool ready = false;
    ~InflateContext() { if (ready) zng_inflateEnd(&stream); }
    void prepare()
    {
        if (ready) { zng_inflateReset(&stream); return; }
        std::memset(&stream, 0, sizeof(stream));
        if (zng_inflateInit2(&stream, -15) != Z_OK) throw Error("zlib-ng inflate initialisation failed");
        ready = true;
    }
};

struct ZstdContexts {
    ZSTD_CCtx *cctx = nullptr;
    ZSTD_DCtx *dctx = nullptr;
    ~ZstdContexts() { ZSTD_freeCCtx(cctx); ZSTD_freeDCtx(dctx); }
};

thread_local DeflateContext t_deflate;
thread_local InflateContext t_inflate;
thread_local ZstdContexts t_zstd;

class CpuBackend final : public Backend {
public:
    explicit CpuBackend(ThreadPool &pool) : m_pool(pool) {}

    BackendInfo info() const override
    {
        BackendInfo i;
        i.name = "CPU (" + std::to_string(m_pool.size()) + " threads)";
        return i;
    }
    bool canCompress(Method m) const override
    {
        return m == Method::Store || m == Method::Deflate || m == Method::LZ4 || m == Method::Zstd;
    }
    bool canDecompress(Method m) const override { return canCompress(m); }
    DeflateShape deflateShape() const override { return DeflateShape::SyncFlushed; }

    static std::size_t bound(Method method, std::size_t size)
    {
        switch (method) {
        case Method::Deflate: return std::size_t(zng_compressBound(size)) + 16; // + sync-flush marker
        case Method::LZ4: return std::size_t(LZ4_compressBound(int(size)));
        case Method::Zstd: return ZSTD_compressBound(size);
        default: return size;
        }
    }

    void compress(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks,
                  ChunkOutput &output) override
    {
        const std::size_t n = chunks.size();
        std::vector<std::size_t> slots(n + 1, 0);
        for (std::size_t i = 0; i < n; ++i) slots[i + 1] = slots[i] + bound(method, chunks[i].size);
        output.data.resize(slots[n]);
        std::vector<std::size_t> sizes(n, 0);
        m_pool.parallelFor(n, [&](std::size_t i) {
            const std::uint8_t *src = input + chunks[i].offset;
            std::uint8_t *dst = output.data.data() + slots[i];
            const std::size_t cap = slots[i + 1] - slots[i];
            sizes[i] = compressOne(method, level, src, chunks[i].size, dst, cap);
        });
        // Compact in place: every chunk moves towards the front, never past
        // the start of a later slot, so a forward pass of memmove is safe.
        output.offsets.assign(n + 1, 0);
        for (std::size_t i = 0; i < n; ++i) {
            output.offsets[i + 1] = output.offsets[i] + sizes[i];
            if (output.offsets[i] != slots[i])
                std::memmove(output.data.data() + output.offsets[i], output.data.data() + slots[i], sizes[i]);
        }
        output.data.resize(output.offsets[n]);
    }

    void decompress(Method method, const std::uint8_t *input, const std::vector<Span> &chunks,
                    std::uint8_t *output, const std::vector<Span> &outputs,
                    std::vector<std::size_t> &actualSizes) override
    {
        actualSizes.assign(chunks.size(), 0);
        m_pool.parallelFor(chunks.size(), [&](std::size_t i) {
            actualSizes[i] = decompressOne(method, input + chunks[i].offset, chunks[i].size,
                                           output + outputs[i].offset, outputs[i].size);
        });
    }

private:
    static std::size_t compressOne(Method method, int level, const std::uint8_t *src, std::size_t size,
                                   std::uint8_t *dst, std::size_t cap)
    {
        switch (method) {
        case Method::Store:
            std::memcpy(dst, src, size);
            return size;
        case Method::Deflate: {
            // Sync flush keeps the chunk byte aligned and non-final, so chunks
            // concatenate into one valid Deflate stream (pigz style).
            auto &ctx = t_deflate;
            ctx.prepare(zlibLevel(level));
            ctx.stream.next_in = src;
            ctx.stream.avail_in = std::uint32_t(size);
            ctx.stream.next_out = dst;
            ctx.stream.avail_out = std::uint32_t(cap);
            const int rc = zng_deflate(&ctx.stream, Z_SYNC_FLUSH);
            if (rc != Z_OK || ctx.stream.avail_in != 0) throw Error("Deflate compression failed");
            return cap - ctx.stream.avail_out;
        }
        case Method::LZ4: {
            const int n = level >= 4
                ? LZ4_compress_HC(reinterpret_cast<const char *>(src), reinterpret_cast<char *>(dst), int(size), int(cap), level >= 5 ? 12 : 9)
                : LZ4_compress_default(reinterpret_cast<const char *>(src), reinterpret_cast<char *>(dst), int(size), int(cap));
            if (n <= 0) throw Error("LZ4 compression failed");
            return std::size_t(n);
        }
        case Method::Zstd: {
            auto &ctx = t_zstd;
            if (!ctx.cctx) ctx.cctx = ZSTD_createCCtx();
            const std::size_t n = ZSTD_compressCCtx(ctx.cctx, dst, cap, src, size, zstdLevel(level));
            if (ZSTD_isError(n)) throw Error(std::string("Zstd compression failed: ") + ZSTD_getErrorName(n));
            return n;
        }
        default:
            throw Error(std::string("CPU backend cannot compress ") + methodName(method));
        }
    }

    static std::size_t decompressOne(Method method, const std::uint8_t *src, std::size_t size,
                                     std::uint8_t *dst, std::size_t cap)
    {
        switch (method) {
        case Method::Store:
            if (size > cap) throw Error("stored chunk larger than expected");
            std::memcpy(dst, src, size);
            return size;
        case Method::Deflate: {
            auto &ctx = t_inflate;
            ctx.prepare();
            ctx.stream.next_in = src;
            ctx.stream.avail_in = std::uint32_t(size);
            ctx.stream.next_out = dst;
            ctx.stream.avail_out = std::uint32_t(cap);
            const int rc = zng_inflate(&ctx.stream, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) throw Error("corrupt Deflate data");
            return cap - ctx.stream.avail_out;
        }
        case Method::LZ4: {
            const int n = LZ4_decompress_safe(reinterpret_cast<const char *>(src), reinterpret_cast<char *>(dst), int(size), int(cap));
            if (n < 0) throw Error("corrupt LZ4 data");
            return std::size_t(n);
        }
        case Method::Zstd: {
            auto &ctx = t_zstd;
            if (!ctx.dctx) ctx.dctx = ZSTD_createDCtx();
            const std::size_t n = ZSTD_decompressDCtx(ctx.dctx, dst, cap, src, size);
            if (ZSTD_isError(n)) throw Error(std::string("corrupt Zstd data: ") + ZSTD_getErrorName(n));
            return n;
        }
        default:
            throw Error(std::string("CPU backend cannot decompress ") + methodName(method));
        }
    }

    ThreadPool &m_pool;
};

} // namespace

std::unique_ptr<Backend> createCpuBackend(ThreadPool &pool) { return std::make_unique<CpuBackend>(pool); }

} // namespace vyra
