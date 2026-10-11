#pragma once

#include "core/common.h"
#include "core/fileio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vyra {

// ---------------------------------------------------------------------------
// ZIP (PKWARE APPNOTE 6.3.x) writer: UTF-8 names, ZIP64 when needed, NTFS
// timestamps. Sizes are patched into the local header after the data is
// written, so no data descriptors are emitted (best compatibility).
// ---------------------------------------------------------------------------
class ZipWriter {
public:
    explicit ZipWriter(OutFile &out) : m_out(out) {}

    struct EntryInfo {
        std::string name; // UTF-8, '/' separated
        bool directory = false;
        Method method = Method::Store; // Store or Deflate
        std::uint64_t size = 0;        // uncompressed size, known up front
        std::uint64_t mtime = 0;       // FILETIME
        std::uint32_t attributes = 0;
    };

    void beginEntry(const EntryInfo &info);
    void writeData(const void *data, std::size_t size);
    void endEntry(std::uint32_t crc);
    void finish();

private:
    struct Record {
        EntryInfo info;
        std::uint64_t localOffset = 0;
        std::uint64_t compressedSize = 0;
        std::uint32_t crc = 0;
        bool zip64Local = false;
    };
    OutFile &m_out;
    std::vector<Record> m_records;
    bool m_open = false;
};

// ---------------------------------------------------------------------------
// ZIP reader (central directory based).
// ---------------------------------------------------------------------------
struct ZipEntry : Entry {
    std::uint16_t methodCode = 0;
    std::uint16_t flags = 0;
    std::uint64_t localHeaderOffset = 0;
};

class ZipReader {
public:
    void open(const fs::path &path);
    const std::vector<ZipEntry> &entries() const { return m_entries; }
    // Offset of the entry's compressed data (reads the local header).
    std::uint64_t dataOffset(InFile &file, const ZipEntry &entry) const;
    const fs::path &path() const { return m_path; }

private:
    fs::path m_path;
    std::vector<ZipEntry> m_entries;
};

Method zipMethodFromCode(std::uint16_t code);

// DOS date/time (local time) <-> FILETIME.
std::uint32_t fileTimeToDos(std::uint64_t fileTime);
std::uint64_t dosToFileTime(std::uint32_t dos);

} // namespace vyra
