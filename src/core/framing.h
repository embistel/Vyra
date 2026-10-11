#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vyra {

// ---- LZ4 frame format (lz4.org, "LZ4 Frame Format Description" v1.6) ----
// Vyra wraps the raw LZ4 blocks that nvCOMP produces into a single standard
// frame with independent blocks, which 7-Zip-zstd / NanaZip / lz4 decode.

// Smallest standard block-maximum (64 KiB, 256 KiB, 1 MiB, 4 MiB) >= size.
std::size_t lz4BlockMaximum(std::size_t chunkSize);
// Writers return the number of bytes stored at `out`.
constexpr std::size_t kLz4FrameHeaderSize = 7;
constexpr std::size_t kLz4EndMarkSize = 4;
std::size_t writeLz4FrameHeader(std::uint8_t *out, std::size_t blockMaximum);
// Size of the block written by writeLz4Block (stored raw when compression did not help).
inline std::size_t lz4BlockSize(std::size_t compressedSize, std::size_t rawSize)
{
    return 4 + (compressedSize < rawSize ? compressedSize : rawSize);
}
std::size_t writeLz4Block(std::uint8_t *out, const std::uint8_t *compressed, std::size_t compressedSize,
                          const std::uint8_t *raw, std::size_t rawSize);
std::size_t writeLz4EndMark(std::uint8_t *out);

// One independently decodable piece of a framed stream.
struct FramedChunk {
    std::size_t offset = 0;   // within the parsed buffer
    std::size_t size = 0;     // compressed bytes
    std::size_t capacity = 0; // maximum decoded bytes
    bool raw = false;         // stored without compression (LZ4 uncompressed block)
};

// Incremental parser for concatenated LZ4 frames or Zstd frames. parse()
// consumes complete chunks from the front of `data` and returns the number
// of bytes consumed; the caller keeps the remainder for the next call.
// `fallback` becomes true at a frame boundary that cannot be split into
// independent chunks (linked LZ4 blocks, Zstd frame without content size);
// everything from that point must go to a streaming decoder instead.
class FrameParser {
public:
    enum class Kind { LZ4, Zstd };
    explicit FrameParser(Kind kind) : m_kind(kind) {}

    std::size_t parse(const std::uint8_t *data, std::size_t size, std::vector<FramedChunk> &chunks);
    bool fallback() const { return m_fallback; }
    bool betweenFrames() const { return m_state == State::FrameStart && m_skip == 0; }

private:
    enum class State { FrameStart, Blocks };
    std::size_t parseLz4(const std::uint8_t *data, std::size_t size, std::vector<FramedChunk> &chunks);
    std::size_t parseZstd(const std::uint8_t *data, std::size_t size, std::vector<FramedChunk> &chunks);

    Kind m_kind;
    State m_state = State::FrameStart;
    bool m_fallback = false;
    std::uint64_t m_skip = 0; // remaining bytes of a skippable frame
    // Current LZ4 frame flags.
    bool m_blockChecksum = false;
    bool m_contentChecksum = false;
    std::size_t m_blockMaximum = 0;
};

} // namespace vyra
