#include "core/streamdecoder.h"
#include "core/backend.h"
#include "core/framing.h"

#include <Bra.h>
#include <Lzma2Dec.h>
#include <LzmaDec.h>
#include <lz4frame.h>
#include <zlib-ng.h>
#include <zstd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace vyra {
namespace {

constexpr std::size_t kOutBlock = 1 << 20;

void *lzmaAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void lzmaFree(ISzAllocPtr, void *address) { std::free(address); }
const ISzAlloc kLzmaAlloc = {lzmaAlloc, lzmaFree};

class CopyDecoder final : public StreamDecoder {
public:
    void feed(const std::uint8_t *data, std::size_t size, bool, const Sink &sink) override
    {
        if (size) sink(data, size);
    }
};

class DeflateDecoder final : public StreamDecoder {
public:
    DeflateDecoder() : m_out(kOutBlock)
    {
        std::memset(&m_stream, 0, sizeof(m_stream));
        if (zng_inflateInit2(&m_stream, -15) != Z_OK) throw Error("zlib-ng inflate initialisation failed");
    }
    ~DeflateDecoder() override { zng_inflateEnd(&m_stream); }
    void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) override
    {
        m_stream.next_in = data;
        m_stream.avail_in = std::uint32_t(size);
        while (!m_finished && (m_stream.avail_in > 0 || end)) {
            m_stream.next_out = m_out.data();
            m_stream.avail_out = std::uint32_t(m_out.size());
            const int rc = zng_inflate(&m_stream, Z_NO_FLUSH);
            const std::size_t produced = m_out.size() - m_stream.avail_out;
            if (produced) sink(m_out.data(), produced);
            if (rc == Z_STREAM_END) { m_finished = true; break; }
            if (rc == Z_BUF_ERROR && produced == 0) {
                if (end) throw Error("truncated Deflate stream");
                break;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) throw Error("corrupt Deflate data");
        }
        if (end && !m_finished) throw Error("truncated Deflate stream");
    }

private:
    zng_stream m_stream;
    std::vector<std::uint8_t> m_out;
    bool m_finished = false;
};

class LzmaDecoder final : public StreamDecoder {
public:
    LzmaDecoder(const std::vector<std::uint8_t> &props, std::uint64_t unpackSize, bool lzma2)
        : m_lzma2(lzma2), m_remaining(unpackSize), m_out(kOutBlock)
    {
        if (lzma2) {
            if (props.size() != 1) throw Error("invalid LZMA2 properties");
            Lzma2Dec_CONSTRUCT(&m_dec2);
            if (Lzma2Dec_Allocate(&m_dec2, props[0], &kLzmaAlloc) != SZ_OK) throw Error("invalid LZMA2 properties");
            Lzma2Dec_Init(&m_dec2);
        } else {
            if (props.size() != 5) throw Error("invalid LZMA properties");
            LzmaDec_CONSTRUCT(&m_dec);
            if (LzmaDec_Allocate(&m_dec, props.data(), 5, &kLzmaAlloc) != SZ_OK) throw Error("invalid LZMA properties");
            LzmaDec_Init(&m_dec);
        }
    }
    ~LzmaDecoder() override
    {
        if (m_lzma2) Lzma2Dec_Free(&m_dec2, &kLzmaAlloc);
        else LzmaDec_Free(&m_dec, &kLzmaAlloc);
    }
    void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) override
    {
        while (m_remaining > 0) {
            SizeT outLen = SizeT(std::min<std::uint64_t>(m_out.size(), m_remaining));
            SizeT inLen = size;
            ELzmaStatus status;
            const SRes rc = m_lzma2
                ? Lzma2Dec_DecodeToBuf(&m_dec2, m_out.data(), &outLen, data, &inLen, LZMA_FINISH_ANY, &status)
                : LzmaDec_DecodeToBuf(&m_dec, m_out.data(), &outLen, data, &inLen, LZMA_FINISH_ANY, &status);
            if (rc != SZ_OK) throw Error("corrupt LZMA data");
            data += inLen;
            size -= inLen;
            if (outLen) {
                sink(m_out.data(), outLen);
                m_remaining -= outLen;
            }
            if (status == LZMA_STATUS_FINISHED_WITH_MARK) break;
            if (inLen == 0 && outLen == 0) {
                if (end) throw Error("truncated LZMA stream");
                break;
            }
        }
        if (end && m_remaining) throw Error("truncated LZMA stream");
    }

private:
    bool m_lzma2;
    CLzmaDec m_dec{};
    CLzma2Dec m_dec2{};
    std::uint64_t m_remaining;
    std::vector<std::uint8_t> m_out;
};

class Lz4StreamDecoder final : public StreamDecoder {
public:
    Lz4StreamDecoder() : m_out(4 << 20)
    {
        if (LZ4F_isError(LZ4F_createDecompressionContext(&m_ctx, LZ4F_VERSION))) throw Error("LZ4 initialisation failed");
    }
    ~Lz4StreamDecoder() override { LZ4F_freeDecompressionContext(m_ctx); }
    void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) override
    {
        while (size > 0) {
            std::size_t outLen = m_out.size();
            std::size_t inLen = size;
            const std::size_t hint = LZ4F_decompress(m_ctx, m_out.data(), &outLen, data, &inLen, nullptr);
            if (LZ4F_isError(hint)) throw Error(std::string("corrupt LZ4 data: ") + LZ4F_getErrorName(hint));
            data += inLen;
            size -= inLen;
            if (outLen) sink(m_out.data(), outLen);
            m_idle = hint == 0;
            if (inLen == 0 && outLen == 0) break;
        }
        // Drain output still buffered inside the context.
        for (;;) {
            std::size_t outLen = m_out.size(), inLen = 0;
            const std::size_t hint = LZ4F_decompress(m_ctx, m_out.data(), &outLen, nullptr, &inLen, nullptr);
            if (LZ4F_isError(hint)) throw Error("corrupt LZ4 data");
            if (!outLen) break;
            sink(m_out.data(), outLen);
        }
        if (end && !m_idle) throw Error("truncated LZ4 stream");
    }

private:
    LZ4F_dctx *m_ctx = nullptr;
    std::vector<std::uint8_t> m_out;
    bool m_idle = true;
};

class ZstdStreamDecoder final : public StreamDecoder {
public:
    ZstdStreamDecoder() : m_out(ZSTD_DStreamOutSize())
    {
        m_ctx = ZSTD_createDStream();
        if (!m_ctx) throw Error("Zstd initialisation failed");
        ZSTD_DCtx_setParameter(m_ctx, ZSTD_d_windowLogMax, 31);
    }
    ~ZstdStreamDecoder() override { ZSTD_freeDStream(m_ctx); }
    void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) override
    {
        ZSTD_inBuffer in{data, size, 0};
        for (;;) {
            ZSTD_outBuffer out{m_out.data(), m_out.size(), 0};
            const std::size_t rc = ZSTD_decompressStream(m_ctx, &out, &in);
            if (ZSTD_isError(rc)) throw Error(std::string("corrupt Zstd data: ") + ZSTD_getErrorName(rc));
            if (out.pos) sink(m_out.data(), out.pos);
            m_frameDone = rc == 0;
            if (in.pos == in.size && out.pos < out.size) break;
        }
        if (end && !m_frameDone) throw Error("truncated Zstd stream");
    }

private:
    ZSTD_DStream *m_ctx = nullptr;
    std::vector<std::uint8_t> m_out;
    bool m_frameDone = true;
};

// Growable byte buffer whose storage comes from the backend (pinned memory
// for the GPU, so uploads run at full speed).
class StagingBuffer {
public:
    explicit StagingBuffer(Backend &backend) : m_backend(backend) {}
    std::uint8_t *data() const { return m_buffer.data(); }
    std::size_t size() const { return m_size; }
    void append(const std::uint8_t *p, std::size_t n)
    {
        reserve(m_size + n);
        std::memcpy(m_buffer.data() + m_size, p, n);
        m_size += n;
    }
    void consume(std::size_t n)
    {
        if (n >= m_size) { m_size = 0; return; }
        std::memmove(m_buffer.data(), m_buffer.data() + n, m_size - n);
        m_size -= n;
    }
    void reserve(std::size_t n)
    {
        if (n <= m_buffer.size()) return;
        HostBuffer grown(m_backend, std::max(n, m_buffer.size() * 2));
        if (m_size) std::memcpy(grown.data(), m_buffer.data(), m_size);
        m_buffer = std::move(grown);
    }

private:
    Backend &m_backend;
    HostBuffer m_buffer;
    std::size_t m_size = 0;
};

// Splits LZ4/Zstd frames into independent chunks and decodes them in large
// batches on the backend. Switches to a streaming decoder at the first frame
// that cannot be split.
class ChunkedDecoder final : public StreamDecoder {
public:
    ChunkedDecoder(Method method, Backend &backend, std::size_t batchBytes)
        : m_method(method), m_backend(backend), m_batchBytes(batchBytes),
          m_parser(method == Method::LZ4 ? FrameParser::Kind::LZ4 : FrameParser::Kind::Zstd),
          m_pending(backend) {}

    void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) override
    {
        if (m_fallback) { m_fallback->feed(data, size, end, sink); return; }
        m_pending.append(data, size);
        if (!end && m_pending.size() < m_batchBytes) return;
        for (;;) {
            m_chunks.clear();
            const std::size_t consumed = m_parser.parse(m_pending.data(), m_pending.size(), m_chunks);
            decodeChunks(sink);
            m_pending.consume(consumed);
            if (m_parser.fallback()) {
                m_fallback = m_method == Method::LZ4 ? std::unique_ptr<StreamDecoder>(new Lz4StreamDecoder)
                                                     : std::unique_ptr<StreamDecoder>(new ZstdStreamDecoder);
                m_fallback->feed(m_pending.data(), m_pending.size(), end, sink);
                m_pending.consume(m_pending.size());
                return;
            }
            if (consumed == 0) break;
            if (!end && m_pending.size() < m_batchBytes) break;
        }
        if (end && (m_pending.size() || !m_parser.betweenFrames())) throw Error("truncated compressed stream");
    }

private:
    void decodeChunks(const Sink &sink)
    {
        std::size_t first = 0;
        while (first < m_chunks.size()) {
            // Group chunks so the decoded batch stays within the batch size.
            std::size_t last = first, capacity = 0;
            while (last < m_chunks.size() && (last == first || capacity + m_chunks[last].capacity <= m_batchBytes))
                capacity += m_chunks[last++].capacity;
            if (m_out.size() < capacity) m_out.reset(m_backend, capacity);
            std::vector<Span> in, out;
            std::vector<std::size_t> index;
            std::vector<std::size_t> produced(last - first, 0), slot(last - first, 0);
            std::size_t offset = 0;
            for (std::size_t i = first; i < last; ++i) {
                const FramedChunk &c = m_chunks[i];
                slot[i - first] = offset;
                if (c.raw) {
                    if (c.size > c.capacity) throw Error("invalid stored block");
                    std::memcpy(m_out.data() + offset, m_pending.data() + c.offset, c.size);
                    produced[i - first] = c.size;
                } else if (c.capacity) {
                    in.push_back({c.offset, c.size});
                    out.push_back({offset, c.capacity});
                    index.push_back(i - first);
                }
                offset += c.capacity;
            }
            if (!in.empty()) {
                std::vector<std::size_t> actual;
                m_backend.decompress(m_method, m_pending.data(), in, m_out.data(), out, actual);
                for (std::size_t k = 0; k < index.size(); ++k) produced[index[k]] = actual[k];
            }
            for (std::size_t k = 0; k < last - first; ++k)
                if (produced[k]) sink(m_out.data() + slot[k], produced[k]);
            first = last;
        }
    }

    Method m_method;
    Backend &m_backend;
    std::size_t m_batchBytes;
    FrameParser m_parser;
    StagingBuffer m_pending;
    HostBuffer m_out;
    std::vector<FramedChunk> m_chunks;
    std::unique_ptr<StreamDecoder> m_fallback;
};

class BcjDecoder final : public StreamDecoder {
public:
    explicit BcjDecoder(std::unique_ptr<StreamDecoder> inner) : m_inner(std::move(inner)) {}
    void feed(const std::uint8_t *data, std::size_t size, bool end, const Sink &sink) override
    {
        m_inner->feed(data, size, end, [&](const std::uint8_t *p, std::size_t n) {
            m_buffer.insert(m_buffer.end(), p, p + n);
            Byte *begin = m_buffer.data();
            Byte *stop = z7_BranchConvSt_X86_Dec(begin, m_buffer.size(), m_pc, &m_state);
            const std::size_t done = std::size_t(stop - begin);
            if (done) {
                sink(m_buffer.data(), done);
                m_pc += std::uint32_t(done);
                m_buffer.erase(m_buffer.begin(), m_buffer.begin() + std::ptrdiff_t(done));
            }
        });
        if (end && !m_buffer.empty()) {
            sink(m_buffer.data(), m_buffer.size()); // trailing bytes shorter than an instruction
            m_buffer.clear();
        }
    }

private:
    std::unique_ptr<StreamDecoder> m_inner;
    std::vector<std::uint8_t> m_buffer;
    std::uint32_t m_pc = 0;
    std::uint32_t m_state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
};

} // namespace

bool canDecode(Method method)
{
    switch (method) {
    case Method::Store: case Method::Deflate: case Method::LZ4: case Method::Zstd:
    case Method::LZMA: case Method::LZMA2: case Method::BCJ:
        return true;
    default:
        return false;
    }
}

std::unique_ptr<StreamDecoder> createStreamDecoder(const DecoderSpec &spec, Backend *backend, std::size_t batchBytes)
{
    switch (spec.method) {
    case Method::Store: return std::make_unique<CopyDecoder>();
    case Method::Deflate: return std::make_unique<DeflateDecoder>();
    case Method::LZMA: return std::make_unique<LzmaDecoder>(spec.properties, spec.unpackSize, false);
    case Method::LZMA2: return std::make_unique<LzmaDecoder>(spec.properties, spec.unpackSize, true);
    case Method::LZ4:
        if (backend) return std::make_unique<ChunkedDecoder>(Method::LZ4, *backend, batchBytes);
        return std::make_unique<Lz4StreamDecoder>();
    case Method::Zstd:
        if (backend) return std::make_unique<ChunkedDecoder>(Method::Zstd, *backend, batchBytes);
        return std::make_unique<ZstdStreamDecoder>();
    default:
        throw Error(std::string("unsupported compression method: ") + methodName(spec.method));
    }
}

std::unique_ptr<StreamDecoder> createBcjDecoder(std::unique_ptr<StreamDecoder> inner)
{
    return std::make_unique<BcjDecoder>(std::move(inner));
}

} // namespace vyra
