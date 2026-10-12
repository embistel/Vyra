#pragma once

#include "core/common.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace vyra {

// Thin RAII wrappers over stdio with 64-bit offsets and wide (UTF-16) paths on
// Windows. Every failure throws vyra::Error naming the file.
class InFile {
public:
    InFile() = default;
    explicit InFile(const fs::path &path) { open(path); }
    ~InFile();
    InFile(const InFile &) = delete;
    InFile &operator=(const InFile &) = delete;

    void open(const fs::path &path);
    void close();
    bool isOpen() const { return m_file != nullptr; }
    std::uint64_t size() const { return m_size; }
    void seek(std::uint64_t offset);
    std::uint64_t tell() const;
    // Reads exactly `size` bytes or throws (premature end of file).
    void readExact(void *data, std::size_t size);
    // Reads up to `size` bytes; returns the count actually read.
    std::size_t readSome(void *data, std::size_t size);
    const fs::path &path() const { return m_path; }

private:
    std::FILE *m_file = nullptr;
    std::uint64_t m_size = 0;
    fs::path m_path;
};

class OutFile {
public:
    OutFile();
    explicit OutFile(const fs::path &path);
    ~OutFile();
    OutFile(const OutFile &) = delete;
    OutFile &operator=(const OutFile &) = delete;

    // direct: for big sequential outputs (archives). Bypasses the file cache
    // and keeps several writes in flight, which is several times faster on
    // fast SSDs than cached writes; falls back to normal writes when the
    // volume does not allow it.
    void create(const fs::path &path, bool direct = false);
    void write(const void *data, std::size_t size);
    // Overwrites bytes at `offset`, then restores the append position. In
    // direct mode the patch is applied when the file is closed.
    void writeAt(std::uint64_t offset, const void *data, std::size_t size);
    std::uint64_t tell() const { return m_position; }
    void close();
    bool isOpen() const { return m_file != nullptr || m_direct != nullptr; }

private:
    struct Direct;
    std::FILE *m_file = nullptr;
    std::unique_ptr<Direct> m_direct;
    std::uint64_t m_position = 0;
    fs::path m_path;
};

struct FileStat {
    bool exists = false;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;      // FILETIME
    std::uint32_t attributes = 0; // FILE_ATTRIBUTE_* bits
};

FileStat statPath(const fs::path &path);
// Positional read with its own handle (safe to call from many threads).
// Returns the number of bytes read; throws if the file cannot be opened.
std::size_t readFileRange(const fs::path &path, std::uint64_t offset, void *data, std::size_t size);
// Best effort; failures are ignored (metadata is not worth failing an extract).
void applyFileMetadata(const fs::path &path, std::uint64_t mtime, std::uint32_t attributes, bool hasAttributes);

std::string systemErrorText();

// Read-only view of part of a file, straight on the file cache (no copy).
// The file stays open with write and delete sharing denied while mapped, so
// it cannot shrink under the view. Only local fixed drives are mapped.
struct FileView {
    const std::uint8_t *data = nullptr; // first byte of the requested range
    void *base = nullptr;               // view start (allocation granularity)
    void *file = nullptr;               // OS handle kept open while mapped
    explicit operator bool() const { return data != nullptr; }
};
// Returns an empty view when mapping is not possible (caller reads instead).
FileView mapFileRange(const fs::path &path, std::uint64_t offset, std::size_t size);
void unmapFileView(FileView &view);

// Whole pages from the OS (not touched yet); freePages releases them.
void *allocatePages(std::size_t bytes);
void freePages(void *pointer);

// Seconds since this process was created (VYRA_PROFILE timelines).
double processUptime();
// Prints "[time] <what> <uptime>" to stderr when VYRA_PROFILE is set.
void profileMark(const char *what);

} // namespace vyra
