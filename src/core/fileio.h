#pragma once

#include "core/common.h"

#include <cstdint>
#include <cstdio>
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
    OutFile() = default;
    explicit OutFile(const fs::path &path) { create(path); }
    ~OutFile();
    OutFile(const OutFile &) = delete;
    OutFile &operator=(const OutFile &) = delete;

    void create(const fs::path &path);
    void write(const void *data, std::size_t size);
    // Overwrites bytes at `offset`, then restores the append position.
    void writeAt(std::uint64_t offset, const void *data, std::size_t size);
    std::uint64_t tell() const { return m_position; }
    void close();
    bool isOpen() const { return m_file != nullptr; }

private:
    std::FILE *m_file = nullptr;
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

} // namespace vyra
