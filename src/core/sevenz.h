#pragma once

#include "core/common.h"
#include "core/fileio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vyra {

// 7z method ids (7-Zip "Methods.txt"). LZ4/Zstd use the ids registered by
// 7-Zip-zstd, which NanaZip and 7-Zip-zstd decode; official 7-Zip decodes
// Copy, Deflate, LZMA, LZMA2 and BCJ.
std::vector<std::uint8_t> sevenZMethodId(Method method);
Method sevenZMethodFromId(const std::vector<std::uint8_t> &id);
// Coder properties written for a method (7-Zip-zstd version/level block).
std::vector<std::uint8_t> sevenZMethodProperties(Method method, int level);

// ---------------------------------------------------------------------------
// Writer. Packed streams are written to the file by the caller between
// begin() and finish(); finish() appends the (plain, uncompressed) header and
// fills in the signature header.
// ---------------------------------------------------------------------------
struct SevenZFileInfo {
    std::string name;
    bool directory = false;
    bool hasStream = false; // false for directories and empty files
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    std::uint32_t attributes = 0;
    std::uint32_t crc = 0;
};

struct SevenZFolderInfo {
    Method method = Method::Store;
    std::vector<std::uint8_t> properties;
    std::uint64_t packSize = 0;
    std::uint64_t unpackSize = 0;
    std::uint64_t streamCount = 0; // number of files in this folder
};

class SevenZWriter {
public:
    explicit SevenZWriter(OutFile &out) : m_out(out) {}
    void begin();
    void finish(const std::vector<SevenZFileInfo> &files, const std::vector<SevenZFolderInfo> &folders);

private:
    OutFile &m_out;
};

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
struct SevenZCoder {
    std::vector<std::uint8_t> id;
    Method method = Method::Other;
    std::vector<std::uint8_t> properties;
    std::uint64_t inStreams = 1;
    std::uint64_t outStreams = 1;
};

struct SevenZFolder {
    std::vector<SevenZCoder> coders;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bindPairs; // (in index, out index)
    std::vector<std::uint64_t> packedStreams;                       // coder in indices fed from pack streams
    std::vector<std::uint64_t> unpackSizes;                         // one per coder out stream
    std::vector<std::uint64_t> packOffsets;                         // absolute file offsets
    std::vector<std::uint64_t> packSizes;
    std::uint64_t substreams = 1;
    bool hasCrc = false;
    std::uint32_t crc = 0;
    std::uint64_t unpackSize() const;
    // Coder ids of the supported decoding chain, top (final output) first.
    std::vector<std::size_t> decodeChain() const;
};

struct SevenZItem : Entry {
    int folder = -1;                    // -1: no data (directory / empty file)
    std::uint64_t offsetInFolder = 0;
};

class SevenZReader {
public:
    void open(const fs::path &path);
    const std::vector<SevenZItem> &items() const { return m_items; }
    const std::vector<SevenZFolder> &folders() const { return m_folders; }
    const fs::path &path() const { return m_path; }

private:
    fs::path m_path;
    std::vector<SevenZItem> m_items;
    std::vector<SevenZFolder> m_folders;
};

// Decodes a whole folder to memory (used for encoded headers, which are small).
std::vector<std::uint8_t> decodeFolderToMemory(InFile &file, const SevenZFolder &folder);

bool sevenZFolderSupported(const SevenZFolder &folder);

} // namespace vyra
