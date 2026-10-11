#pragma once

#include "core/common.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vyra {

struct CompressOptions {
    Format format = Format::SevenZip;
    Method method = Method::LZ4;
    int level = 1;                                  // 0 (fastest) .. 5 (best)
    BackendKind backend = BackendKind::Auto;
    int gpuDevice = 0;
    std::size_t chunkSize = 64 * 1024;              // independent compression unit
    std::size_t batchSize = 128u << 20;             // bytes per GPU batch
    std::uint64_t solidBlockSize = 4ull << 30;      // 7z only; 0 = one block per file
    int threads = 0;                                // 0 = all hardware threads
    bool discardOutput = false;                     // benchmark: encode but write nothing
};

struct ExtractOptions {
    BackendKind backend = BackendKind::Auto;
    int gpuDevice = 0;
    int threads = 0;
    std::size_t batchSize = 128u << 20;
    bool testOnly = false;                 // decode and verify CRCs, write nothing
    std::vector<std::string> selection;    // archive paths (files or folders); empty = all
    bool overwrite = true;                 // false: keep existing files
};

enum class Phase { Scanning, Compressing, Extracting, Testing, Finalizing };

struct Progress {
    Phase phase = Phase::Scanning;
    std::string currentItem;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t filesDone = 0;
    std::uint64_t filesTotal = 0;
    std::uint64_t outputBytes = 0;
    double elapsedSeconds = 0;
    std::string backend;
};

// Called periodically from the calling thread. Return false to cancel.
using ProgressCallback = std::function<bool(const Progress &)>;

struct OperationResult {
    std::uint64_t inputBytes = 0;
    std::uint64_t outputBytes = 0;
    std::uint64_t files = 0;
    double seconds = 0;
    std::string backend;
    std::vector<std::string> warnings;
};

struct ArchiveListing {
    Format format = Format::Zip;
    std::vector<Entry> entries;
    std::uint64_t archiveSize = 0;
    std::string methods; // e.g. "LZ4" or "Deflate, LZMA2"
    bool solid = false;
};

// Creates `archive` from files/folders. Each input becomes a top-level entry
// named after itself, like 7-Zip's "a" command. Throws vyra::Error /
// vyra::Cancelled; a partially written archive is removed.
OperationResult createArchive(const fs::path &archive, const std::vector<fs::path> &inputs,
                              const CompressOptions &options, const ProgressCallback &progress = {});

Format detectFormat(const fs::path &archive);
ArchiveListing listArchive(const fs::path &archive);

OperationResult extractArchive(const fs::path &archive, const fs::path &destination,
                               const ExtractOptions &options, const ProgressCallback &progress = {});

// Archive path -> safe relative filesystem path ("" if it must be skipped).
fs::path safeRelativePath(const std::string &archivePath);

bool methodSupportedFor(Format format, Method method);

} // namespace vyra
