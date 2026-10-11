#pragma once

#include <cstddef>
#include <cstdint>

namespace vyra {

// Standard CRC-32 (PKZIP / 7z), SIMD accelerated through zlib-ng.
std::uint32_t crc32Update(std::uint32_t crc, const void *data, std::size_t size);
inline std::uint32_t crc32(const void *data, std::size_t size) { return crc32Update(0, data, size); }
// CRC of A||B from crc(A), crc(B) and len(B).
std::uint32_t crc32Combine(std::uint32_t crcA, std::uint32_t crcB, std::uint64_t lengthB);

} // namespace vyra
