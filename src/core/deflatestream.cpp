#include "core/deflatestream.h"
#include "core/threadpool.h"

#include <cstring>

namespace vyra {
namespace {

class BitReader {
public:
    BitReader(const std::uint8_t *data, std::size_t size) : m_data(data), m_size(size) {}

    void refill()
    {
        if (m_cnt > 56) return;
        if (m_pos + 8 <= m_size) {
            std::uint64_t word;
            std::memcpy(&word, m_data + m_pos, 8); // little-endian hosts only (x86/ARM)
            m_buf |= word << m_cnt;
            m_pos += (63 - m_cnt) >> 3;
            m_cnt |= 56;
            return;
        }
        while (m_cnt <= 56) {
            const std::uint64_t b = m_pos < m_size ? m_data[m_pos] : 0;
            m_buf |= b << m_cnt;
            ++m_pos;
            m_cnt += 8;
        }
    }
    std::uint32_t peek(int n) const { return std::uint32_t(m_buf & ((1ull << n) - 1)); }
    void consume(int n) { m_buf >>= n; m_cnt -= n; }
    std::uint32_t bits(int n)
    {
        if (n == 0) return 0;
        refill();
        const std::uint32_t v = peek(n);
        consume(n);
        return v;
    }
    std::size_t bitPosition() const { return m_pos * 8 - std::size_t(m_cnt); }
    void seekBit(std::size_t bit)
    {
        m_pos = bit / 8;
        m_buf = 0;
        m_cnt = 0;
        refill();
        consume(int(bit % 8));
    }
    void alignToByte() { consume(m_cnt & 7); }
    bool overrun() const { return bitPosition() > m_size * 8; }

private:
    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
    std::uint64_t m_buf = 0;
    int m_cnt = 0;
};

constexpr int kLutBits = 10;

struct Huffman {
    std::uint16_t lut[1 << kLutBits];
    std::uint16_t count[16];
    std::uint16_t symbols[320];

    bool build(const std::uint8_t *lengths, int n)
    {
        std::memset(count, 0, sizeof(count));
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; // over-subscribed
        }
        std::uint16_t offs[17];
        offs[1] = 0;
        for (int len = 1; len < 16; ++len) offs[len + 1] = std::uint16_t(offs[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbols[offs[lengths[s]]++] = std::uint16_t(s);
        std::memset(lut, 0, sizeof(lut));
        unsigned code = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            for (int k = 0; k < count[len]; ++k, ++code) {
                const std::uint16_t sym = symbols[index++];
                if (len > kLutBits) continue;
                unsigned rev = 0;
                for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1u) << (len - 1 - b);
                for (unsigned j = rev; j < (1u << kLutBits); j += 1u << len)
                    lut[j] = std::uint16_t((sym << 4) | len);
            }
            code <<= 1;
        }
        return true;
    }

    int decode(BitReader &br) const
    {
        br.refill();
        const std::uint16_t e = lut[br.peek(kLutBits)];
        if (e) {
            br.consume(e & 15);
            return e >> 4;
        }
        // Long code: canonical decode bit by bit (as in zlib's puff.c).
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

const std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct FixedTables {
    Huffman lit, dist;
    FixedTables()
    {
        std::uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        lit.build(l, 288);
        std::uint8_t d[30];
        std::memset(d, 5, sizeof(d));
        dist.build(d, 30);
    }
};

bool skipSymbols(BitReader &br, const Huffman &lit, const Huffman &dist)
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

bool readDynamicTables(BitReader &br, Huffman &lit, Huffman &dist)
{
    static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int hlit = int(br.bits(5)) + 257;
    const int hdist = int(br.bits(5)) + 1;
    const int hclen = int(br.bits(4)) + 4;
    if (hlit > 286 || hdist > 30) return false;
    std::uint8_t clen[19] = {};
    for (int i = 0; i < hclen; ++i) clen[order[i]] = std::uint8_t(br.bits(3));
    Huffman cl;
    if (!cl.build(clen, 19)) return false;
    std::uint8_t lengths[320] = {};
    int n = 0;
    while (n < hlit + hdist) {
        const int sym = cl.decode(br);
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
    if (lengths[256] == 0) return false; // no end-of-block code
    return lit.build(lengths, hlit) && dist.build(lengths + hlit, hdist);
}

} // namespace

DeflateScan scanDeflate(const std::uint8_t *data, std::size_t size)
{
    static const FixedTables fixed;
    DeflateScan result;
    BitReader br(data, size);
    Huffman lit, dist;
    for (;;) {
        const std::size_t header = br.bitPosition();
        const bool final = br.bits(1) != 0;
        const std::uint32_t type = br.bits(2);
        if (type == 0) {
            br.alignToByte();
            const std::uint32_t len = br.bits(16);
            const std::uint32_t nlen = br.bits(16);
            if ((len ^ 0xFFFFu) != nlen) return result;
            br.seekBit(br.bitPosition() + std::size_t(len) * 8);
        } else if (type == 1) {
            if (!skipSymbols(br, fixed.lit, fixed.dist)) return result;
        } else if (type == 2) {
            if (!readDynamicTables(br, lit, dist) || !skipSymbols(br, lit, dist)) return result;
        } else {
            return result;
        }
        if (br.overrun()) return result;
        if (final) {
            result.ok = true;
            result.lastHeaderBit = header;
            result.endBit = br.bitPosition();
            return result;
        }
    }
}

void joinDeflateChunks(ChunkOutput &chunks, const std::vector<std::uint8_t> &streamEnd, DeflateShape shape,
                       ThreadPool &pool, ChunkOutput &scratch)
{
    const std::size_t n = chunks.count();
    if (n == 0) return;
    std::vector<DeflateScan> scans(n);
    std::vector<std::size_t> sizes(n);
    pool.parallelFor(n, [&](std::size_t i) {
        const std::size_t size = chunks.chunkSize(i);
        if (shape == DeflateShape::SyncFlushed) {
            sizes[i] = size + (streamEnd[i] ? 2 : 0);
            return;
        }
        if (streamEnd[i]) { sizes[i] = size; return; }
        scans[i] = scanDeflate(chunks.chunk(i), size);
        if (!scans[i].ok || (scans[i].endBit + 7) / 8 > size)
            throw Error("GPU produced a Deflate chunk that could not be parsed");
        const std::size_t bytes = (scans[i].endBit + 7) / 8;
        const unsigned r = unsigned(scans[i].endBit % 8);
        sizes[i] = bytes + ((r == 0 || r > 5) ? 5 : 4);
    });

    ChunkOutput &joined = scratch;
    joined.offsets.assign(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) joined.offsets[i + 1] = joined.offsets[i] + sizes[i];
    joined.data.resize(joined.offsets[n]);
    pool.parallelFor(n, [&](std::size_t i) {
        std::uint8_t *dst = joined.data.data() + joined.offsets[i];
        const std::uint8_t *src = chunks.chunk(i);
        const std::size_t size = chunks.chunkSize(i);
        if (shape == DeflateShape::SyncFlushed) {
            std::memcpy(dst, src, size);
            // Final fixed-Huffman block holding only end-of-block: 1,01,0000000.
            if (streamEnd[i]) { dst[size] = 0x03; dst[size + 1] = 0x00; }
            return;
        }
        if (streamEnd[i]) { std::memcpy(dst, src, size); return; }
        const DeflateScan &s = scans[i];
        const std::size_t bytes = (s.endBit + 7) / 8;
        std::memcpy(dst, src, bytes);
        dst[s.lastHeaderBit / 8] &= std::uint8_t(~(1u << (s.lastHeaderBit % 8))); // clear BFINAL
        const unsigned r = unsigned(s.endBit % 8);
        if (r) dst[bytes - 1] &= std::uint8_t((1u << r) - 1); // zero the padding bits
        std::uint8_t *tail = dst + bytes;
        // Empty stored block: 3 header bits (0,00) land in the zeroed padding
        // when they fit, otherwise in one extra zero byte; then LEN=0, NLEN=~0.
        if (r == 0 || r > 5) *tail++ = 0x00;
        tail[0] = 0x00; tail[1] = 0x00; tail[2] = 0xFF; tail[3] = 0xFF;
    });
    std::swap(chunks.offsets, joined.offsets);
    chunks.data.swap(joined.data);
    chunks.sizes.clear(); // joined output is contiguous
}

} // namespace vyra
