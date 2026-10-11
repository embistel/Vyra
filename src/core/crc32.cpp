#include "core/crc32.h"

#include <zlib-ng.h>

#include <algorithm>

namespace vyra {

std::uint32_t crc32Update(std::uint32_t crc, const void *data, std::size_t size)
{
    const auto *p = static_cast<const std::uint8_t *>(data);
    while (size) {
        const auto piece = std::uint32_t(std::min<std::size_t>(size, 1u << 30));
        crc = zng_crc32(crc, p, piece);
        p += piece;
        size -= piece;
    }
    return crc;
}

std::uint32_t crc32Combine(std::uint32_t crcA, std::uint32_t crcB, std::uint64_t lengthB)
{
    return std::uint32_t(zng_crc32_combine(crcA, crcB, static_cast<z_off64_t>(lengthB)));
}

} // namespace vyra
