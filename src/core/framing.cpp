#include "core/framing.h"
#include "core/bytes.h"
#include "core/common.h"

#include <algorithm>
#include <cstring>

namespace vyra {
namespace {

constexpr std::uint32_t kLz4Magic = 0x184D2204u;
constexpr std::uint32_t kZstdMagic = 0xFD2FB528u;
// Larger frames decode faster through a streaming decoder than as one GPU chunk.
constexpr std::uint64_t kMaxChunkCapacity = 1ull << 26;

bool isSkippable(std::uint32_t magic) { return (magic & 0xFFFFFFF0u) == 0x184D2A50u; }

std::uint32_t rotl(std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

// XXH32 for inputs shorter than 16 bytes (the LZ4 frame descriptor).
std::uint32_t xxh32Short(const std::uint8_t *p, std::size_t len)
{
    constexpr std::uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    std::uint32_t h = P5 + std::uint32_t(len);
    std::size_t i = 0;
    for (; i + 4 <= len; i += 4) h = rotl(h + getLE32(p + i) * P3, 17) * P4;
    for (; i < len; ++i) h = rotl(h + p[i] * P5, 11) * P1;
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

} // namespace

std::size_t lz4BlockMaximum(std::size_t chunkSize)
{
    for (std::size_t m : {std::size_t(64) << 10, std::size_t(256) << 10, std::size_t(1) << 20})
        if (chunkSize <= m) return m;
    return std::size_t(4) << 20;
}

std::size_t writeLz4FrameHeader(std::uint8_t *out, std::size_t blockMaximum)
{
    std::uint8_t bd = 4;
    if (blockMaximum > (64u << 10)) bd = 5;
    if (blockMaximum > (256u << 10)) bd = 6;
    if (blockMaximum > (1u << 20)) bd = 7;
    setLE32(out, kLz4Magic);
    out[4] = 0x60; // version 01, independent blocks, no checksums
    out[5] = std::uint8_t(bd << 4);
    out[6] = std::uint8_t((xxh32Short(out + 4, 2) >> 8) & 0xFF);
    return kLz4FrameHeaderSize;
}

std::size_t writeLz4Block(std::uint8_t *out, const std::uint8_t *compressed, std::size_t compressedSize,
                          const std::uint8_t *raw, std::size_t rawSize)
{
    if (compressedSize >= rawSize) {
        setLE32(out, std::uint32_t(rawSize) | 0x80000000u);
        std::memcpy(out + 4, raw, rawSize);
        return 4 + rawSize;
    }
    setLE32(out, std::uint32_t(compressedSize));
    std::memcpy(out + 4, compressed, compressedSize);
    return 4 + compressedSize;
}

std::size_t writeLz4EndMark(std::uint8_t *out)
{
    setLE32(out, 0);
    return kLz4EndMarkSize;
}

std::size_t FrameParser::parse(const std::uint8_t *data, std::size_t size, std::vector<FramedChunk> &chunks)
{
    std::size_t pos = 0;
    // Skippable frames may span several parse() calls.
    if (m_skip) {
        const std::size_t n = std::size_t(std::min<std::uint64_t>(m_skip, size));
        m_skip -= n;
        pos += n;
        if (m_skip) return pos;
    }
    if (m_fallback) return pos;
    return pos + (m_kind == Kind::LZ4 ? parseLz4(data + pos, size - pos, chunks) : parseZstd(data + pos, size - pos, chunks));
}

std::size_t FrameParser::parseLz4(const std::uint8_t *data, std::size_t size, std::vector<FramedChunk> &chunks)
{
    std::size_t pos = 0;
    while (!m_fallback) {
        if (m_state == State::FrameStart) {
            if (size - pos < 4) break;
            const std::uint32_t magic = getLE32(data + pos);
            if (isSkippable(magic)) {
                if (size - pos < 8) break;
                m_skip = getLE32(data + pos + 4);
                pos += 8;
                const std::size_t n = std::size_t(std::min<std::uint64_t>(m_skip, size - pos));
                m_skip -= n;
                pos += n;
                if (m_skip) break;
                continue;
            }
            if (magic != kLz4Magic) throw Error("invalid LZ4 frame");
            if (size - pos < 7) break;
            const std::uint8_t flg = data[pos + 4];
            const std::uint8_t bd = data[pos + 5];
            if ((flg >> 6) != 1) throw Error("unsupported LZ4 frame version");
            const bool independent = (flg & 0x20) != 0;
            const bool hasContentSize = (flg & 0x08) != 0;
            const bool hasDict = (flg & 0x01) != 0;
            const std::size_t headerSize = 7 + (hasContentSize ? 8 : 0) + (hasDict ? 4 : 0);
            if (size - pos < headerSize) break;
            if (!independent || hasDict) { m_fallback = true; break; }
            const int id = (bd >> 4) & 7;
            if (id < 4) throw Error("invalid LZ4 block size");
            m_blockMaximum = std::size_t(1) << (8 + 2 * id);
            m_blockChecksum = (flg & 0x10) != 0;
            m_contentChecksum = (flg & 0x04) != 0;
            pos += headerSize;
            m_state = State::Blocks;
        } else {
            if (size - pos < 4) break;
            const std::uint32_t word = getLE32(data + pos);
            if (word == 0) {
                const std::size_t need = 4 + (m_contentChecksum ? 4 : 0);
                if (size - pos < need) break;
                pos += need;
                m_state = State::FrameStart;
                continue;
            }
            const std::size_t blockSize = word & 0x7FFFFFFFu;
            const std::size_t need = 4 + blockSize + (m_blockChecksum ? 4 : 0);
            if (blockSize > m_blockMaximum + 64) throw Error("invalid LZ4 block");
            if (size - pos < need) break;
            FramedChunk c;
            c.offset = pos + 4;
            c.size = blockSize;
            c.capacity = m_blockMaximum;
            c.raw = (word & 0x80000000u) != 0;
            chunks.push_back(c);
            pos += need;
        }
    }
    return pos;
}

std::size_t FrameParser::parseZstd(const std::uint8_t *data, std::size_t size, std::vector<FramedChunk> &chunks)
{
    std::size_t pos = 0;
    while (!m_fallback) {
        if (size - pos < 4) break;
        const std::uint32_t magic = getLE32(data + pos);
        if (isSkippable(magic)) {
            if (size - pos < 8) break;
            m_skip = getLE32(data + pos + 4);
            pos += 8;
            const std::size_t n = std::size_t(std::min<std::uint64_t>(m_skip, size - pos));
            m_skip -= n;
            pos += n;
            if (m_skip) break;
            continue;
        }
        if (magic != kZstdMagic) throw Error("invalid Zstd frame");
        if (size - pos < 5) break;
        const std::uint8_t fhd = data[pos + 4];
        const int fcsFlag = fhd >> 6;
        const bool singleSegment = (fhd & 0x20) != 0;
        const bool checksum = (fhd & 0x04) != 0;
        static const int dictSizes[4] = {0, 1, 2, 4};
        static const int fcsSizes[4] = {0, 2, 4, 8};
        const int dictBytes = dictSizes[fhd & 3];
        const int fcsBytes = fcsFlag == 0 ? (singleSegment ? 1 : 0) : fcsSizes[fcsFlag];
        const std::size_t headerSize = 5 + (singleSegment ? 0 : 1) + std::size_t(dictBytes) + std::size_t(fcsBytes);
        if (size - pos < headerSize) break;
        if (fcsBytes == 0 || dictBytes != 0) { m_fallback = true; break; }
        const std::uint8_t *f = data + pos + headerSize - fcsBytes;
        std::uint64_t contentSize = 0;
        switch (fcsBytes) {
        case 1: contentSize = f[0]; break;
        case 2: contentSize = getLE16(f) + 256u; break;
        case 4: contentSize = getLE32(f); break;
        default: contentSize = getLE64(f); break;
        }
        if (contentSize > kMaxChunkCapacity) { m_fallback = true; break; }
        // Walk the blocks to find the end of the frame.
        std::size_t q = pos + headerSize;
        bool complete = false;
        for (;;) {
            if (size - q < 3) break;
            const std::uint32_t bh = std::uint32_t(data[q]) | (std::uint32_t(data[q + 1]) << 8) | (std::uint32_t(data[q + 2]) << 16);
            const bool last = (bh & 1) != 0;
            const std::uint32_t type = (bh >> 1) & 3;
            const std::size_t bsize = bh >> 3;
            if (type == 3) throw Error("invalid Zstd block");
            const std::size_t content = type == 1 ? 1 : bsize;
            if (size - q - 3 < content) break;
            q += 3 + content;
            if (last) {
                if (checksum) {
                    if (size - q < 4) break;
                    q += 4;
                }
                complete = true;
                break;
            }
        }
        if (!complete) break;
        FramedChunk c;
        c.offset = pos;
        c.size = q - pos;
        c.capacity = std::size_t(contentSize);
        chunks.push_back(c);
        pos = q;
    }
    return pos;
}

} // namespace vyra
