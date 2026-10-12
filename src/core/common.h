#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace vyra {

namespace fs = std::filesystem;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Cancelled final : public Error {
public:
    Cancelled() : Error("operation cancelled") {}
};

// Zip and 7z hold many files; gz (gzip) and lz4 (LZ4 frame) hold exactly
// one file each, like the gzip and lz4 command-line tools.
enum class Format { Zip, SevenZip, Gzip, Lz4 };

inline bool singleFileFormat(Format format) { return format == Format::Gzip || format == Format::Lz4; }

// Coding methods. Store/Deflate/LZ4/Zstd can be written; the rest are only
// decoded (or merely listed) when reading archives made by other tools.
enum class Method { Store, Deflate, LZ4, Zstd, LZMA, LZMA2, BCJ, Other };

enum class BackendKind { Auto, Gpu, Cpu };

const char *formatName(Format format);
const char *methodName(Method method);

// One item of an archive listing, independent of the container format.
struct Entry {
    std::string path;             // UTF-8, '/' separated, no trailing slash
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t packedSize = 0; // 0 when not known per file (7z solid blocks)
    std::uint64_t mtime = 0;      // FILETIME, 0 = unknown
    std::uint32_t attributes = 0;
    bool hasAttributes = false;
    std::uint32_t crc = 0;
    bool hasCrc = false;
    Method method = Method::Store;
    bool encrypted = false;
    bool supported = true;        // false when Vyra cannot decode the method
};

// UTF-8 <-> filesystem path helpers. Archive paths are always UTF-8 with '/'.
inline fs::path pathFromUtf8(const std::string &utf8) { return fs::u8path(utf8); }
inline std::string utf8FromPath(const fs::path &path) { return path.u8string(); }

// FILETIME: 100 ns ticks since 1601-01-01 UTC. Used as the canonical
// timestamp because both 7z and NTFS store it natively.
constexpr std::uint64_t kFileTimeUnixEpoch = 116444736000000000ULL;

} // namespace vyra
