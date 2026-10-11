#pragma once

#include "core/backend.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vyra {

class ThreadPool;

struct DeflateScan {
    bool ok = false;
    std::size_t lastHeaderBit = 0; // bit offset of the BFINAL flag of the final block
    std::size_t endBit = 0;        // bit offset just past the final block's end-of-block code
};

// Walks the blocks of a raw Deflate stream without producing output and
// reports where the final block starts and ends.
DeflateScan scanDeflate(const std::uint8_t *data, std::size_t size);

// Makes independently compressed Deflate chunks concatenate into one valid
// raw Deflate stream (RFC 1951). streamEnd[i] marks the last chunk of each
// stream. Non-final chunks end in an empty stored block (a sync-flush marker),
// final chunks end in a BFINAL block. Any inflater, including 7-Zip, Windows
// Explorer and zlib, decodes the result as a single ordinary stream.
// `scratch` receives the previous storage so it can be reused next time.
void joinDeflateChunks(ChunkOutput &chunks, const std::vector<std::uint8_t> &streamEnd, DeflateShape shape,
                       ThreadPool &pool, ChunkOutput &scratch);

} // namespace vyra
