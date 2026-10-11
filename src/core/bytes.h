#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace vyra {

inline std::uint16_t getLE16(const std::uint8_t *p) { return std::uint16_t(p[0] | (p[1] << 8)); }
inline std::uint32_t getLE32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline std::uint64_t getLE64(const std::uint8_t *p) { return getLE32(p) | (std::uint64_t(getLE32(p + 4)) << 32); }

inline void setLE16(std::uint8_t *p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void setLE32(std::uint8_t *p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i));
}
inline void setLE64(std::uint8_t *p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = std::uint8_t(v >> (8 * i));
}

// Append-only little-endian byte writer.
class ByteWriter {
public:
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { std::uint8_t b[2]; setLE16(b, v); raw(b, 2); }
    void u32(std::uint32_t v) { std::uint8_t b[4]; setLE32(b, v); raw(b, 4); }
    void u64(std::uint64_t v) { std::uint8_t b[8]; setLE64(b, v); raw(b, 8); }
    void raw(const void *data, std::size_t size)
    {
        const auto *p = static_cast<const std::uint8_t *>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    std::size_t size() const { return bytes.size(); }
};

} // namespace vyra
