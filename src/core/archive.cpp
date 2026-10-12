#include "core/archive.h"
#include "core/backend.h"
#include "core/bytes.h"
#include "core/crc32.h"
#include "core/deflatestream.h"
#include "core/fileio.h"
#include "core/framing.h"
#include "core/sevenz.h"
#include "core/streamdecoder.h"
#include "core/threadpool.h"
#include "core/zip.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <future>
#include <optional>
#include <set>
#include <thread>

namespace vyra {
namespace {

using Clock = std::chrono::steady_clock;
// Reads are split into pieces so a batch keeps every I/O thread busy.
constexpr std::size_t kReadPiece = 4u << 20;

std::string lowerAscii(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

template<class T> class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : m_capacity(capacity) {}
    bool push(T value)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
        if (m_closed) return false;
        m_items.push_back(std::move(value));
        m_notEmpty.notify_one();
        return true;
    }
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [&] { return m_closed || !m_items.empty(); });
        if (m_items.empty()) return std::nullopt;
        T value = std::move(m_items.front());
        m_items.pop_front();
        m_notFull.notify_one();
        return value;
    }
    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }
    std::size_t size()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }
    std::optional<T> tryPop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty()) return std::nullopt;
        T value = std::move(m_items.front());
        m_items.pop_front();
        m_notFull.notify_one();
        return value;
    }

private:
    std::size_t m_capacity;
    std::deque<T> m_items;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty, m_notFull;
    bool m_closed = false;
};

fs::path nullDevice()
{
#ifdef _WIN32
    return fs::path(L"NUL");
#else
    return fs::path("/dev/null");
#endif
}

struct SharedState {
    std::atomic<std::uint64_t> bytesDone{0}, bytesTotal{0}, filesDone{0}, filesTotal{0}, outputBytes{0};
    std::atomic<bool> cancel{false};
    std::atomic<int> phase{int(Phase::Scanning)};
    std::mutex mutex;
    std::string currentItem;
    std::string backend;

    void setItem(const std::string &item)
    {
        std::lock_guard<std::mutex> lock(mutex);
        currentItem = item;
    }
    void checkCancel() const
    {
        if (cancel.load()) throw Cancelled();
    }
    Progress snapshot(Clock::time_point start)
    {
        Progress p;
        p.phase = Phase(phase.load());
        p.bytesDone = bytesDone;
        p.bytesTotal = bytesTotal;
        p.filesDone = filesDone;
        p.filesTotal = filesTotal;
        p.outputBytes = outputBytes;
        p.elapsedSeconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::lock_guard<std::mutex> lock(mutex);
        p.currentItem = currentItem;
        p.backend = backend;
        return p;
    }
};

// Runs `work` on a worker thread while the calling thread reports progress,
// so callbacks always arrive on the caller's thread.
template<class Fn> double runWithProgress(SharedState &state, const ProgressCallback &callback, Fn work)
{
    const auto start = Clock::now();
    std::exception_ptr error;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread worker([&] {
        try {
            work();
        } catch (...) {
            error = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!done) {
            cv.wait_for(lock, std::chrono::milliseconds(100));
            if (done) break;
            if (callback) {
                lock.unlock();
                if (!callback(state.snapshot(start))) state.cancel = true;
                lock.lock();
            }
        }
    }
    worker.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (callback && !error) callback(state.snapshot(start));
    if (error) std::rethrow_exception(error);
    return seconds;
}

// gzip extra subfield written by Vyra: the exact 64-bit uncompressed size
// (ISIZE in the trailer is only the size modulo 4 GiB).
constexpr std::uint8_t kGzipSizeId1 = 'V', kGzipSizeId2 = 'y';

// ---------------------------------------------------------------------------
// Input scanning
// ---------------------------------------------------------------------------
struct Item {
    fs::path source;
    std::string name;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    std::uint32_t attributes = 0;
    int stream = -1; // ZIP entry stream or 7z folder, -1 = no data
};

std::vector<Item> scanInputs(const std::vector<fs::path> &inputs, const fs::path &archive, SharedState &state,
                             std::vector<std::string> &warnings)
{
    std::error_code ec;
    const std::string archiveKey = lowerAscii(fs::absolute(archive, ec).lexically_normal().generic_u8string());
    std::vector<Item> items;
    auto add = [&](const fs::path &path, const std::string &name) {
        const FileStat st = statPath(path);
        if (!st.exists) {
            warnings.push_back("cannot read: " + utf8FromPath(path));
            return;
        }
        if (!st.directory && lowerAscii(fs::absolute(path, ec).lexically_normal().generic_u8string()) == archiveKey) return;
        Item item;
        item.source = path;
        item.name = name;
        item.directory = st.directory;
        item.size = st.size;
        item.mtime = st.mtime;
        item.attributes = st.attributes;
        state.bytesTotal += st.size;
        ++state.filesTotal;
        items.push_back(std::move(item));
    };
    for (const fs::path &input : inputs) {
        state.checkCancel();
        fs::path root = fs::absolute(input, ec).lexically_normal();
        if (!root.has_filename()) root = root.parent_path();
        std::string rootName = utf8FromPath(root.filename());
        if (rootName.empty() || rootName.back() == ':') rootName = "root";
        add(root, rootName);
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) {
            warnings.push_back("cannot list: " + utf8FromPath(root));
            continue;
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                warnings.push_back("cannot list: " + ec.message());
                break;
            }
            state.checkCancel();
            const fs::path &p = it->path();
            add(p, rootName + "/" + p.lexically_relative(root).generic_u8string());
            if ((items.size() & 1023) == 0) state.setItem(items.back().name);
        }
    }
    return items;
}

// ---------------------------------------------------------------------------
// Compression pipeline: reader thread -> codec thread -> writer.
// ---------------------------------------------------------------------------
struct Segment {
    std::size_t item;
    std::size_t offset;
    std::size_t size;
    bool first;
    bool last;
};

struct ReadTask {
    std::size_t item;
    std::uint64_t fileOffset;
    std::size_t bufferOffset;
    std::size_t size;
};

struct Batch {
    void reset()
    {
        buffer = -1;
        data = nullptr;
        used = 0;
        chunks.clear();
        streamStart.clear();
        streamEnd.clear();
        chunkStream.clear();
        segments.clear();
        segmentCrc.clear();
        reads.clear();
        output.clear();
        framed = false;
        unmapFileView(view);
    }
    std::uint64_t sequence = 0;
    bool framed = false; // output already carries the container framing
    FileView view;       // data maps the file directly (CPU jobs, big files)
    double stamps[6] = {}; // VYRA_PROFILE=2: planned, read, codec start/end, posted, written
    DeflateShape shape = DeflateShape::SyncFlushed;
    int buffer = -1;
    std::uint8_t *data = nullptr;
    std::size_t used = 0;
    std::vector<Span> chunks;
    std::vector<std::uint8_t> streamStart, streamEnd;
    std::vector<std::uint32_t> chunkStream;
    std::vector<Segment> segments;
    std::vector<std::uint32_t> segmentCrc;
    std::vector<ReadTask> reads;
    ChunkOutput output;
};

// Segment CRCs are computed in pieces of at most 4 MiB (in parallel, on the
// CPU or the GPU) and combined per segment.
struct CrcPieces {
    std::vector<Span> ranges;
    std::vector<std::size_t> segment;
    std::vector<std::uint32_t> crcs;
};

void planCrcPieces(const Batch &b, CrcPieces &p)
{
    constexpr std::size_t kPiece = 4u << 20;
    p.ranges.clear();
    p.segment.clear();
    p.crcs.clear();
    for (std::size_t s = 0; s < b.segments.size(); ++s)
        for (std::size_t off = 0; off < b.segments[s].size; off += kPiece) {
            p.ranges.push_back({b.segments[s].offset + off, std::min(kPiece, b.segments[s].size - off)});
            p.segment.push_back(s);
        }
}

// The same, but pieces never cross a chunk boundary, so a CPU backend can
// checksum each piece together with compressing its chunk.
void planChunkCrcPieces(const Batch &b, CrcPieces &p)
{
    p.ranges.clear();
    p.segment.clear();
    p.crcs.clear();
    std::size_t c = 0;
    for (std::size_t s = 0; s < b.segments.size(); ++s) {
        std::size_t off = b.segments[s].offset;
        const std::size_t end = off + b.segments[s].size;
        while (off < end) {
            while (c < b.chunks.size() && b.chunks[c].offset + b.chunks[c].size <= off) ++c;
            if (c == b.chunks.size()) throw Error("internal error: segment outside the chunks");
            const std::size_t pieceEnd = std::min(end, b.chunks[c].offset + b.chunks[c].size);
            p.ranges.push_back({off, pieceEnd - off});
            p.segment.push_back(s);
            off = pieceEnd;
        }
    }
}

void combineCrcPieces(Batch &b, const CrcPieces &p)
{
    b.segmentCrc.assign(b.segments.size(), 0);
    for (std::size_t i = 0; i < p.ranges.size(); ++i) {
        auto &c = b.segmentCrc[p.segment[i]];
        c = crc32Combine(c, p.crcs[i], p.ranges[i].size);
    }
}

// Input staging memory: plain pages, so files can be read while the GPU is
// still starting up; registered with the backend (page-locked) once it is
// there, so uploads run as direct DMA.
class InputBuffer {
public:
    explicit InputBuffer(std::size_t bytes) : m_data(static_cast<std::uint8_t *>(allocatePages(bytes))), m_size(bytes) {}
    ~InputBuffer()
    {
        if (m_registered) m_registered->unregisterHost(m_data);
        freePages(m_data);
    }
    InputBuffer(const InputBuffer &) = delete;
    InputBuffer &operator=(const InputBuffer &) = delete;
    std::uint8_t *data() const { return m_data; }
    void registerWith(Backend &backend)
    {
        if (!m_registered && backend.registerHost(m_data, m_size)) m_registered = &backend;
    }

private:
    std::uint8_t *m_data;
    std::size_t m_size;
    Backend *m_registered = nullptr;
};

void computeSegmentCrcs(Batch &b, ThreadPool &pool)
{
    CrcPieces p;
    planCrcPieces(b, p);
    p.crcs.resize(p.ranges.size());
    pool.parallelFor(p.ranges.size(), [&](std::size_t i) { p.crcs[i] = crc32(b.data + p.ranges[i].offset, p.ranges[i].size); });
    combineCrcPieces(b, p);
}

// Wraps raw LZ4 blocks into LZ4 frames (header at stream start, end mark at
// stream end). `scratch` receives the previous storage for reuse.
void frameLz4(Batch &b, std::size_t blockMaximum, ThreadPool &pool, ChunkOutput &scratch)
{
    const std::size_t n = b.chunks.size();
    ChunkOutput &framed = scratch;
    framed.offsets.assign(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        framed.offsets[i + 1] = framed.offsets[i] + (b.streamStart[i] ? kLz4FrameHeaderSize : 0) +
                                lz4BlockSize(b.output.chunkSize(i), b.chunks[i].size) + (b.streamEnd[i] ? kLz4EndMarkSize : 0);
    }
    framed.data.resize(framed.offsets[n]);
    pool.parallelFor(n, [&](std::size_t i) {
        std::uint8_t *out = framed.data.data() + framed.offsets[i];
        if (b.streamStart[i]) out += writeLz4FrameHeader(out, blockMaximum);
        out += writeLz4Block(out, b.output.chunk(i), b.output.chunkSize(i), b.data + b.chunks[i].offset, b.chunks[i].size);
        if (b.streamEnd[i]) writeLz4EndMark(out);
    });
    std::swap(b.output.offsets, framed.offsets);
    b.output.data.swap(framed.data);
    b.output.sizes.clear(); // framed output is contiguous
}

// Parallel reads out of the file cache (copies or page mapping) stop scaling
// at a few threads (kernel locks); more only take cores from compression.
int ioThreadCount() { return std::min(8, defaultThreadCount()); }

// Hands chunks [from, to) to `write`, merging runs that are adjacent in
// memory (all of them for contiguous output).
template<class Fn> void writeChunks(const ChunkOutput &out, std::size_t from, std::size_t to, Fn write)
{
    while (from < to) {
        const std::uint8_t *start = out.chunk(from);
        std::size_t size = out.chunkSize(from);
        while (++from < to && out.chunk(from) == start + size) size += out.chunkSize(from);
        if (size) write(start, size);
    }
}

// Per-stage wall time, printed to stderr when VYRA_PROFILE is set.
struct StageTimer {
    std::atomic<std::int64_t> micros{0};
    struct Scope {
        StageTimer &t;
        Clock::time_point start = Clock::now();
        ~Scope() { t.micros += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count(); }
    };
    double seconds() const { return double(micros.load()) / 1e6; }
};

class Compressor {
public:
    Compressor(const fs::path &archive, const CompressOptions &options, SharedState &state, bool eachFile = false)
        : m_archive(archive), m_options(options), m_state(state), m_pool(options.threads), m_eachFile(eachFile) {}

    OperationResult run(const std::vector<fs::path> &inputs, const ProgressCallback &progress)
    {
        OperationResult result;
        const fs::path temp = fs::path(m_archive).concat(".vyra-partial");
        try {
            result.seconds = runWithProgress(m_state, progress, [&] {
                profileMark("job start");
                const auto t0 = Clock::now();
                m_items = scanInputs(inputs, m_archive, m_state, result.warnings);
                if (singleFileFormat(m_options.format)) planSingleFileOutputs(result.warnings);
                startBackend();
                const auto t1 = Clock::now();
                prepareLayout();
                const auto t2 = Clock::now();
                m_state.phase = int(Phase::Compressing);
                const bool archiveFile = !singleFileFormat(m_options.format);
                if (archiveFile) m_out.create(m_options.discardOutput ? nullDevice() : temp, !m_options.discardOutput);
                pipeline();
                profileMark("pipeline done");
                const auto t3 = Clock::now();
                m_state.phase = int(Phase::Finalizing);
                if (!archiveFile) return; // each output was renamed by the writer
                m_out.close();
                if (std::getenv("VYRA_PROFILE")) {
                    auto sec = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
                    std::fprintf(stderr, "[profile] scan %.3fs prepare %.3fs pipeline %.3fs close %.3fs\n", sec(t0, t1), sec(t1, t2),
                                 sec(t2, t3), sec(t3, Clock::now()));
                }
                if (m_options.discardOutput) return;
                std::error_code ec;
                fs::rename(temp, m_archive, ec);
                if (ec) {
                    fs::remove(m_archive, ec);
                    fs::rename(temp, m_archive, ec);
                    if (ec) throw Error("cannot create " + utf8FromPath(m_archive) + ": " + ec.message());
                }
            });
        } catch (...) {
            if (m_out.isOpen()) {
                try { m_out.close(); } catch (...) {}
            }
            std::error_code ec;
            if (!m_options.discardOutput && !singleFileFormat(m_options.format)) fs::remove(temp, ec);
            throw;
        }
        result.inputBytes = m_state.bytesTotal;
        result.outputBytes = m_state.outputBytes;
        result.files = singleFileFormat(m_options.format) ? m_state.filesDone.load() : m_items.size();
        result.backend = m_state.backend;
        return result;
    }

private:
    // Starts the GPU backend in the background when the job may use it: CUDA
    // start-up takes about half a second, during which the input is scanned
    // and the first batches are read.
    void startBackend()
    {
        const Method method = m_options.method;
        if (!methodSupportedFor(m_options.format, method)) throw Error(std::string(methodName(method)) + " is not available for this format");
        // nvCOMP's high-ratio Deflate modes are much slower than zlib-ng on
        // a multi-core CPU, so Auto keeps those levels on the CPU.
        const bool cpuOnly = method == Method::Store || m_options.backend == BackendKind::Cpu ||
                             (m_options.backend == BackendKind::Auto && method == Method::Deflate && m_options.level >= 3);
        m_wantGpu = !cpuOnly && (m_options.backend == BackendKind::Gpu || gpuLikelyFaster());
        if (!m_wantGpu) return;
        const int device = m_options.gpuDevice;
        m_gpuInit = std::async(std::launch::async, [device] {
            std::string reason;
            std::unique_ptr<Backend> gpu = createGpuBackend(device, &reason);
            return std::make_pair(std::move(gpu), reason);
        });
    }

    // gz / lz4 hold one file each: keep regular files only and choose each
    // one's output. Each-file mode skips files that already carry the
    // extension and outputs that exist (like gzip); a single explicit
    // output (createArchive) must be given exactly one file.
    void planSingleFileOutputs(std::vector<std::string> &warnings)
    {
        const std::string ext = std::string(".") + formatName(m_options.format);
        std::vector<Item> kept;
        std::uint64_t bytes = 0;
        for (Item &item : m_items) {
            if (item.directory) continue;
            fs::path out = fs::path(item.source).concat(ext);
            if (m_eachFile) {
                // Already compressed by one of these formats: nothing to gain.
                const std::string sourceExt = lowerAscii(utf8FromPath(item.source.extension()));
                if (sourceExt == ".gz" || sourceExt == ".tgz" || sourceExt == ".lz4") {
                    warnings.push_back(utf8FromPath(item.source) + ": already compressed, skipped");
                    continue;
                }
                std::error_code ec;
                if (fs::exists(out, ec)) {
                    warnings.push_back(utf8FromPath(out) + ": exists, not overwritten");
                    continue;
                }
            } else {
                out = m_archive;
            }
            bytes += item.size;
            m_outputs.push_back(out);
            kept.push_back(std::move(item));
        }
        if (!m_eachFile && kept.size() != 1)
            throw Error(std::string(formatName(m_options.format)) + " holds exactly one file; compress folders or several files to zip or 7z, or each file separately");
        m_items = std::move(kept);
        m_state.bytesTotal = bytes;
        m_state.filesTotal = m_items.size();
    }

    // Auto engine choice. Small jobs: CUDA start-up (~0.5 s) would dominate.
    // Otherwise compress a sample of the actual input on one CPU thread and
    // scale by the thread count: a many-core CPU outruns the GPU pipeline
    // for fast codecs (the GPU path also pays PCIe transfers and start-up).
    bool gpuLikelyFaster()
    {
        if (const char *e = std::getenv("VYRA_AUTO")) return std::string(e) == "gpu"; // tuning / tests
        constexpr std::uint64_t kSmallJob = std::uint64_t(256) << 20;
        if (m_state.bytesTotal < kSmallJob) return false;
        // Sample: up to 1 MiB from the start of the largest file.
        const Item *largest = nullptr;
        for (const Item &item : m_items)
            if (!item.directory && (!largest || item.size > largest->size)) largest = &item;
        if (!largest || largest->size < 65536) return false;
        const std::size_t sampleSize = std::size_t(std::min<std::uint64_t>(largest->size, std::uint64_t(1) << 20)) & ~std::size_t(65535);
        std::vector<std::uint8_t> sample(sampleSize);
        if (readFileRange(largest->source, 0, sample.data(), sampleSize) != sampleSize) return true;
        std::vector<Span> chunks;
        for (std::size_t off = 0; off < sampleSize; off += 65536) chunks.push_back({off, 65536});
        ThreadPool single(1);
        auto cpu = createCpuBackend(single);
        ChunkOutput out;
        cpu->compress(m_options.method, m_options.level, sample.data(), chunks, out); // warm up
        double seconds = 1e9; // best of two: other load on the machine only adds time
        for (int round = 0; round < 2; ++round) {
            const auto start = Clock::now();
            cpu->compress(m_options.method, m_options.level, sample.data(), chunks, out);
            seconds = std::min(seconds, std::max(1e-6, std::chrono::duration<double>(Clock::now() - start).count()));
        }
        // 70% of linear scaling: memory bandwidth and the other pipeline
        // stages share the cores. ~2 GiB/s is what the GPU path delivers end
        // to end in a fresh process (CUDA start-up, transfers, writing).
        const double cpuRate = double(sampleSize) / seconds * m_pool.size() * 0.7;
        constexpr double kGpuRate = 2.0 * double(1u << 30);
        if (std::getenv("VYRA_PROFILE"))
            std::fprintf(stderr, "[auto] CPU estimate %.2f GiB/s (%d threads) vs GPU %.2f GiB/s\n", cpuRate / double(1u << 30),
                         m_pool.size(), kGpuRate / double(1u << 30));
        return cpuRate < kGpuRate;
    }

    // Waits for the backend chosen by startBackend (or falls back to the CPU)
    // and lets it prepare for the job.
    void attachBackend()
    {
        const Method method = m_options.method;
        if (m_gpuInit.valid()) {
            auto started = m_gpuInit.get();
            if (started.first && started.first->canCompress(method)) m_backend = std::move(started.first);
            else if (m_options.backend == BackendKind::Gpu) throw Error("GPU backend unavailable: " + started.second);
        }
        if (!m_backend) m_backend = createCpuBackend(m_pool);
        profileMark("backend ready");
        if (method != Method::Store) {
            try {
                m_outputBound = m_backend->prepareCompress(method, m_options.level, m_chunkSize, m_batchSize);
            } catch (const Error &) {
                // The GPU cannot hold this job (device memory): Auto continues on the CPU.
                if (m_options.backend != BackendKind::Auto || !m_backend->info().gpu) throw;
                m_backend = createCpuBackend(m_pool);
                m_outputBound = 0;
            }
        }
        profileMark("prepared");
        // As many codec workers as the backend takes calls at once: the GPU
        // backend overlaps one batch's upload with another's compression and
        // a third one's download. Post-processing (only needed when the
        // backend cannot frame on the device) runs in its own stage.
        m_codecs.assign(std::size_t(std::max(1, m_backend->concurrency())), m_backend.get());
        std::string engine = method == Method::Store ? "Store (no compression)" : m_backend->info().name;
        if (m_hybrid && m_backend->info().gpu) engine += m_hybrid == 1 ? " (CPU during start-up)" : " + CPU";
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.backend = engine;
    }

    void prepareLayout()
    {
        const Method method = m_options.method;
        m_chunkSize = std::clamp<std::size_t>(m_options.chunkSize, 4096, method == Method::Deflate ? 65536 : (4u << 20));
        m_chunkSize &= ~std::size_t(255);
        sizeBatches();

        // Assign streams: one per non-empty file (zip) or solid folders (7z).
        int stream = -1;
        std::uint64_t folderBytes = 0;
        for (Item &item : m_items) {
            if (item.directory || item.size == 0) continue;
            if (m_options.format == Format::Zip || singleFileFormat(m_options.format)) {
                item.stream = ++stream;
                continue;
            }
            const bool split = stream < 0 || m_options.solidBlockSize == 0 || folderBytes + item.size > m_options.solidBlockSize;
            if (split && !(stream >= 0 && folderBytes == 0)) {
                ++stream;
                folderBytes = 0;
                SevenZFolderInfo f;
                f.method = method;
                f.properties = sevenZMethodProperties(method, levelFor7z(method));
                m_folders.push_back(f);
            }
            item.stream = stream;
            folderBytes += item.size;
            m_folders.back().unpackSize += item.size;
            ++m_folders.back().streamCount;
        }
    }

    static int hybridMode()
    {
        if (const char *e = std::getenv("VYRA_HYBRID")) return std::clamp(std::atoi(e), 0, 3);
        return 2;
    }

    void sizeBatches()
    {
        // GPUs want big batches (fewer, larger transfers and launches); the CPU
        // is fastest when a batch stays close to its caches.
        // 64 MiB keeps the pinned memory small (it is slow to set up and to
        // release), and with four batches in flight the GPU still overlaps
        // transfers and compression.
        const std::size_t automatic = m_wantGpu ? (std::size_t(64) << 20) : (std::size_t(32) << 20);
        std::size_t batch = std::max<std::size_t>(m_options.batchSize ? m_options.batchSize : automatic, 4 * m_chunkSize);
        // Never stage more than the input needs (small jobs stay small).
        batch = std::size_t(std::min<std::uint64_t>(batch, m_state.bytesTotal + m_items.size() * 512 + 65536));
        m_batchSize = std::max<std::size_t>(batch, 1 << 16);
    }

    int levelFor7z(Method method) const
    {
        static const int zstd[] = {1, 1, 3, 6, 12, 19};
        if (method == Method::Zstd) return zstd[std::clamp(m_options.level, 0, 5)];
        return 1;
    }

    void fail(std::exception_ptr e)
    {
        {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            if (!m_error) m_error = e;
        }
        m_state.cancel = true;
        m_readQueue.close();
        m_postQueue.close();
        m_writeQueue.close();
        m_freeBuffers.close();
        m_freeBatches.close();
    }

    void pipeline()
    {
        // Input buffers are recycled as soon as a batch is encoded; batch
        // objects (holding the encoded output) once the writer is done.
        // Inputs go back to the reader as soon as they are uploaded (GPU) or
        // encoded; outputs grow to what the data needs. Pinned memory is
        // expensive to set up and tear down, so keep both pools small.
        // CPU jobs mostly map the input (no buffer memory is touched then), so
        // more batches can be in flight than buffers would allow.
        // Batches finish out of order and are written in order, so extra batch
        // objects keep a slow batch from stalling the reader.
        const int kBuffers = m_wantGpu ? 6 : 8, kBatches = 2 * kBuffers;
        // Read-ahead allowance while the GPU starts (about 1.5 GiB).
        m_maxInputs = m_wantGpu ? std::max<std::size_t>(kBuffers, (std::size_t(1536) << 20) / m_batchSize) : std::size_t(kBuffers);
        m_growInputs = m_wantGpu;
        m_mapInputs = !m_wantGpu;
        for (int i = 0; i < kBuffers; ++i) {
            m_inputs.push_back(std::make_unique<InputBuffer>(m_batchSize));
            m_freeBuffers.push(i);
        }
        for (int i = 0; i < kBatches; ++i) m_freeBatches.push(std::make_unique<Batch>());
        // Reading starts right away, while the backend may still be starting.
        std::vector<std::thread> unmappers;
        for (int k = 0; k < 1; ++k)
            unmappers.emplace_back([this] {
                while (auto view = m_unmapQueue.pop()) unmapFileView(*view);
            });
        std::thread reader([this] {
            raiseThreadPriority();
            try { readerLoop(); } catch (...) { fail(std::current_exception()); }
            m_readQueue.close();
        });
        // Codec workers take batches from the read queue and finish out of
        // order (the writer restores it). With a GPU, a CPU worker starts at
        // once (hybrid): CUDA start-up takes about half a second, and the
        // CPU compresses meanwhile. GPU workers join once the backend is up.
        std::atomic<int> codecsRunning{1}; // the starter keeps the post queue open
        std::vector<std::thread> codecs;
        auto finishCodec = [this, &codecsRunning] {
            if (--codecsRunning == 0) m_postQueue.close();
        };
        auto runCodec = [this, finishCodec](Backend *backend) {
            try { codecLoop(*backend); } catch (...) { fail(std::current_exception()); }
            finishCodec();
        };
        m_hybrid = m_wantGpu ? hybridMode() : 0;
        if (m_hybrid) {
            m_cpuBackend = createCpuBackend(m_pool);
            ++codecsRunning;
            codecs.emplace_back(runCodec, m_cpuBackend.get());
        }
        std::mutex codecsMutex;
        std::thread starter([&] {
            try {
                attachBackend();
                {
                    std::lock_guard<std::mutex> lock(m_inputsMutex);
                    m_growInputs = false;
                    if (std::getenv("VYRA_PROFILE")) std::fprintf(stderr, "[profile] %zu input buffers at backend start\n", m_inputs.size());
                }
                // Input buffers are page-locked by the reader just before it
                // fills one (never while a read may be writing into it).
                if (m_backend->info().gpu) m_registerWith = m_backend.get();
                // Outputs of GPU batches start at half a batch (typical
                // compressed data fits); incompressible data grows them once.
                // Page-locked memory costs time to set up and to release.
                m_initialOutput = m_outputBound ? std::min(m_outputBound, m_batchSize / 2 + (std::size_t(1) << 20)) : 0;
                profileMark("host buffers ready");
                const bool gpu = m_backend->info().gpu;
                if (gpu || !m_hybrid) {
                    std::lock_guard<std::mutex> lock(codecsMutex);
                    m_gpuStarted = gpu;
                    for (Backend *b : m_codecs) {
                        ++codecsRunning;
                        codecs.emplace_back(runCodec, b);
                    }
                }
            } catch (...) {
                fail(std::current_exception());
            }
            m_starterDone = true;
            finishCodec();
        });
        std::thread post([this] {
            try { postLoop(); } catch (...) { fail(std::current_exception()); }
            m_writeQueue.close();
        });
        try {
            writerLoop();
        } catch (...) {
            fail(std::current_exception());
        }
        reader.join();
        starter.join();
        for (auto &t : codecs) t.join();
        post.join();
        m_unmapQueue.close();
        for (auto &t : unmappers) t.join();
        m_freeBatches.close();
        while (m_freeBatches.pop()) {}
        m_inputs.clear();
        if (std::getenv("VYRA_PROFILE")) {
            std::fprintf(stderr, "[profile] read %.2fs (wait buf %.2fs) | crc %.2fs compress %.2fs post %.2fs (wait input %.2fs) | write %.2fs (wait %.2fs)\n",
                         m_tRead.seconds(), m_tReadWait.seconds(), m_tCrc.seconds(), m_tCompress.seconds(), m_tPost.seconds(),
                         m_tCodecWait.seconds(), m_tWrite.seconds(), m_tWriteWait.seconds());
            std::fprintf(stderr, "[profile] %d batches read through file mapping\n", m_mappedBatches);
        }
        if (m_error) std::rethrow_exception(m_error);
        if (m_state.cancel) throw Cancelled();
    }

    std::unique_ptr<Batch> newBatch()
    {
        StageTimer::Scope timing{m_tReadWait};
        auto index = m_freeBuffers.tryPop();
        if (!index) {
            // While the GPU is still starting up, keep reading ahead into
            // extra buffers instead of waiting (they are registered with the
            // backend once it is ready).
            std::lock_guard<std::mutex> lock(m_inputsMutex);
            if (m_growInputs && m_inputs.size() < m_maxInputs) {
                m_inputs.push_back(std::make_unique<InputBuffer>(m_batchSize));
                index = int(m_inputs.size() - 1);
                m_freeBatches.push(std::make_unique<Batch>());
            }
        }
        if (!index) index = m_freeBuffers.pop();
        if (!index) throw Cancelled();
        auto pooled = m_freeBatches.pop();
        if (!pooled) throw Cancelled();
        auto b = std::move(*pooled);
        b->buffer = *index;
        {
            std::lock_guard<std::mutex> lock(m_inputsMutex);
            InputBuffer &input = *m_inputs[std::size_t(*index)];
            if (Backend *gpu = m_registerWith.load()) input.registerWith(*gpu);
            b->data = input.data();
        }
        return b;
    }

    void readerLoop()
    {
        auto batch = newBatch();
        std::uint64_t sequence = 0;
        std::size_t chunkOpen = 0;
        bool pendingStart = false;
        int current = -1;
        auto pushChunk = [&](std::size_t offset, std::size_t size, bool end) {
            batch->chunks.push_back({offset, size});
            batch->streamStart.push_back(pendingStart && m_options.format != Format::Lz4 ? 1 : 0);
            batch->streamEnd.push_back(end ? 1 : 0);
            batch->chunkStream.push_back(std::uint32_t(current));
            pendingStart = false;
        };
        auto closeChunk = [&](bool end) {
            if (batch->used > chunkOpen) {
                pushChunk(chunkOpen, batch->used - chunkOpen, end);
                chunkOpen = batch->used;
            } else if (end) {
                if (batch->chunks.empty()) throw Error("internal error: stream end without data");
                batch->streamEnd.back() = 1;
            }
        };
        auto submit = [&] {
            if (m_timeline) batch->stamps[0] = processUptime();
            executeReads(*batch);
            if (m_timeline) batch->stamps[1] = processUptime();
            if (!m_readQueue.push(std::move(batch))) throw Cancelled();
            batch = newBatch();
            batch->sequence = ++sequence;
            chunkOpen = 0;
        };
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            m_state.checkCancel();
            const Item &item = m_items[i];
            if (item.stream < 0) {
                batch->segments.push_back({i, batch->used, 0, true, true});
                continue;
            }
            if (item.stream != current) {
                if (current >= 0) closeChunk(true);
                if (m_batchSize - batch->used < 4096 + 256) submit();
                batch->used = std::min(m_batchSize, (batch->used + 255) & ~std::size_t(255));
                chunkOpen = batch->used;
                current = item.stream;
                pendingStart = true;
            }
            std::uint64_t remaining = item.size;
            bool first = true;
            while (remaining) {
                if (batch->used >= m_batchSize) {
                    closeChunk(false);
                    submit();
                }
                const std::size_t n = std::size_t(std::min<std::uint64_t>({remaining, m_batchSize - batch->used, kReadPiece}));
                // Reads are only planned here and run in parallel at submit
                // time: overlapping opens and copies matters for many small files.
                batch->reads.push_back({i, item.size - remaining, batch->used, n});
                batch->segments.push_back({i, batch->used, n, first, remaining == n});
                first = false;
                batch->used += n;
                remaining -= n;
                while (batch->used - chunkOpen >= m_chunkSize) {
                    pushChunk(chunkOpen, m_chunkSize, false);
                    chunkOpen += m_chunkSize;
                }
                m_state.checkCancel();
            }
            if (m_options.format == Format::Zip || singleFileFormat(m_options.format)) {
                closeChunk(true);
                current = -1;
            }
        }
        if (current >= 0) closeChunk(true);
        executeReads(*batch);
        if (!m_readQueue.push(std::move(batch))) throw Cancelled();
    }

    // A batch that is one contiguous range of one file can use the file
    // cache pages directly (CPU jobs): no copy, which is most of the reading
    // cost on a machine short of memory bandwidth.
    bool mapBatch(Batch &b)
    {
        if (!m_mapInputs || b.reads.empty() || b.reads.front().bufferOffset != 0) return false;
        // Only a share of the batches: mapping and unmapping serialise on the
        // process's page tables, copies on the file cache; mixing both reads
        // faster than either alone. Copy while the unmap thread is behind, so
        // its backlog (waited for at the end of the job) stays small.
        if (m_unmapQueue.size() >= 3) return false;
        m_mapCredit += m_mapShare;
        if (m_mapCredit < 100) return false;
        m_mapCredit -= 100;
        const ReadTask &first = b.reads.front();
        std::uint64_t fileEnd = first.fileOffset;
        std::size_t bufferEnd = 0;
        for (const ReadTask &t : b.reads) {
            if (t.item != first.item || t.fileOffset != fileEnd || t.bufferOffset != bufferEnd) return false;
            fileEnd += t.size;
            bufferEnd += t.size;
        }
        if (bufferEnd != b.used || bufferEnd < (std::size_t(4) << 20)) return false;
        FileView view = mapFileRange(m_items[first.item].source, first.fileOffset, bufferEnd);
        if (!view) return false;
        b.view = view;
        b.data = const_cast<std::uint8_t *>(view.data); // read-only use
        ++m_mappedBatches;
        // Map the pages here, on the I/O threads (one read per page), so the
        // compression threads do not take a soft fault per page later.
        const std::size_t pages = (bufferEnd + 4095) / 4096, parts = 16;
        m_ioPool.parallelFor(parts, [&](std::size_t part) {
            volatile std::uint8_t sink = 0;
            for (std::size_t p = pages * part / parts; p < pages * (part + 1) / parts; ++p) sink = sink + view.data[p * 4096];
            (void)sink;
        });
        return true;
    }

    void executeReads(Batch &b)
    {
        StageTimer::Scope timing{m_tRead};
        if (mapBatch(b)) return;
        m_ioPool.parallelFor(b.reads.size(), [&](std::size_t k) {
            m_state.checkCancel();
            const ReadTask &t = b.reads[k];
            const Item &item = m_items[t.item];
            if (readFileRange(item.source, t.fileOffset, b.data + t.bufferOffset, t.size) != t.size)
                throw Error("file changed while it was being read: " + utf8FromPath(item.source));
        });
    }

    void releaseInput(Batch &b)
    {
        // Unmapping a view costs ~10 ms per 32 MiB (page tables, TLB
        // shootdowns); a background thread does it so no stage waits.
        if (b.view) {
            m_unmapQueue.push(b.view);
            b.view = FileView{};
        }
        if (b.buffer >= 0) m_freeBuffers.push(b.buffer);
        b.buffer = -1;
        b.data = nullptr;
    }

    // GPU path: compression, container framing and CRCs in one device pass.
    bool compressOnDevice(Backend &backend, Batch &b, CrcPieces &crc)
    {
        planCrcPieces(b, crc);
        CompressExtras extras;
        extras.inputReleased = [&] { releaseInput(b); };
        extras.streamStart = &b.streamStart;
        extras.streamEnd = &b.streamEnd;
        extras.lz4BlockMaximum = lz4BlockMaximum(m_chunkSize);
        extras.crcRanges = &crc.ranges;
        extras.crcs = &crc.crcs;
        {
            StageTimer::Scope timing{m_tCompress};
            if (!backend.compressFramed(m_options.method, m_options.level, b.data, b.chunks, extras, b.output)) return false;
        }
        b.framed = true;
        if (crc.crcs.size() != crc.ranges.size()) {
            StageTimer::Scope timing{m_tCrc};
            computeSegmentCrcs(b, m_pool);
        } else {
            combineCrcPieces(b, crc);
        }
        return true;
    }

    void codecLoop(Backend &backend)
    {
        const Method method = m_options.method;
        const bool gpu = backend.info().gpu;
        CrcPieces crcPieces;
        for (;;) {
            // Hybrid mode 1: the CPU only bridges the GPU start-up. Mode 3
            // (tests): the CPU starts with the GPU, so their batches mix.
            if (!gpu && m_hybrid == 1 && m_gpuStarted) break;
            while (!gpu && m_hybrid == 3 && !m_starterDone && !m_state.cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::optional<std::unique_ptr<Batch>> next;
            {
                StageTimer::Scope timing{m_tCodecWait};
                next = m_readQueue.pop();
            }
            if (!next) break;
            Batch &b = **next;
            if (m_timeline) b.stamps[2] = processUptime();
            if (gpu) {
                // GPU batches need page-locked outputs from the GPU backend.
                if (b.output.data.allocator() != &backend) {
                    b.output.data.rebind(&backend);
                    if (m_initialOutput) {
                        b.output.data.resize(m_initialOutput);
                        b.output.data.clear();
                    }
                }
            } else if (b.output.data.capacity() == 0) {
                b.output.data.setAllocator(&backend);
            }
            if (method == Method::Store) {
                computeSegmentCrcs(b, m_pool);
                b.output.offsets.assign(b.chunks.size() + 1, 0);
                for (std::size_t i = 0; i < b.chunks.size(); ++i) b.output.offsets[i + 1] = b.output.offsets[i] + b.chunks[i].size;
                b.output.data.resize(b.output.offsets.back());
                m_pool.parallelFor(b.chunks.size(), [&](std::size_t i) {
                    std::memcpy(b.output.data.data() + b.output.offsets[i], b.data + b.chunks[i].offset, b.chunks[i].size);
                });
            } else if (gpu && compressOnDevice(backend, b, crcPieces)) {
                // Framed (and usually CRC'd) on the GPU: the input buffer goes
                // back to the reader (if the backend did not release it yet).
                releaseInput(b);
            } else if (gpu) {
                // CRCs run on the CPU while the GPU compresses.
                std::exception_ptr crcError;
                std::thread crc([&] {
                    StageTimer::Scope timing{m_tCrc};
                    try { computeSegmentCrcs(b, m_pool); } catch (...) { crcError = std::current_exception(); }
                });
                try {
                    StageTimer::Scope timing{m_tCompress};
                    backend.compress(method, m_options.level, b.data, b.chunks, b.output);
                } catch (...) {
                    crc.join();
                    throw;
                }
                crc.join();
                if (crcError) std::rethrow_exception(crcError);
            } else {
                // CPU: CRCs fused with compression, chunk by chunk; LZ4 also
                // framed in place.
                planChunkCrcPieces(b, crcPieces);
                {
                    StageTimer::Scope timing{m_tCompress};
                    CompressExtras extras;
                    extras.streamStart = &b.streamStart;
                    extras.streamEnd = &b.streamEnd;
                    extras.lz4BlockMaximum = lz4BlockMaximum(m_chunkSize);
                    extras.crcRanges = &crcPieces.ranges;
                    extras.crcs = &crcPieces.crcs;
                    if (backend.compressFramed(method, m_options.level, b.data, b.chunks, extras, b.output)) b.framed = true;
                    else backend.compressChecked(method, m_options.level, b.data, b.chunks, crcPieces.ranges, crcPieces.crcs, b.output);
                }
                combineCrcPieces(b, crcPieces);
            }
            b.shape = backend.deflateShape();
            if (m_timeline) b.stamps[3] = processUptime();
            if (!m_postQueue.push(std::move(*next))) throw Cancelled();
        }
    }

    // Container framing that needs the CPU: Deflate stream joining and LZ4
    // frames. Runs while the codec already works on the next batch.
    void postLoop()
    {
        const Method method = m_options.method;
        while (auto next = m_postQueue.pop()) {
            Batch &b = **next;
            if (!b.framed) {
                StageTimer::Scope postTiming{m_tPost};
                if (method == Method::Deflate) joinDeflateChunks(b.output, b.streamEnd, b.shape, m_pool, m_scratch);
                if (method == Method::LZ4) frameLz4(b, lz4BlockMaximum(m_chunkSize), m_pool, m_scratch);
            }
            releaseInput(b);
            if (m_timeline) b.stamps[4] = processUptime();
            if (!m_writeQueue.push(std::move(*next))) throw Cancelled();
        }
    }

    // Batches finish out of order; hands them out in sequence. False at the end.
    bool nextBatchInOrder(std::unique_ptr<Batch> &batch)
    {
        for (;;) {
            auto found = m_early.find(m_expected);
            if (found != m_early.end()) {
                batch = std::move(found->second);
                m_early.erase(found);
                ++m_expected;
                return true;
            }
            std::optional<std::unique_ptr<Batch>> next;
            {
                StageTimer::Scope timing{m_tWriteWait};
                next = m_writeQueue.pop();
            }
            if (!next) {
                if (!m_early.empty() && !m_error && !m_state.cancel) throw Error("internal error: missing batch");
                return false;
            }
            if ((*next)->sequence != m_expected) {
                const std::uint64_t seq = (*next)->sequence;
                m_early.emplace(seq, std::move(*next));
                continue;
            }
            batch = std::move(*next);
            ++m_expected;
            return true;
        }
    }

    void recycleBatch(std::unique_ptr<Batch> batch)
    {
        if (m_timeline) {
            const double *t = batch->stamps;
            std::fprintf(stderr, "[batch] %3llu plan %7.3f read %7.3f codec %7.3f..%7.3f post %7.3f written %7.3f\n",
                         static_cast<unsigned long long>(batch->sequence), t[0], t[1], t[2], t[3], t[4], processUptime());
        }
        batch->reset();
        m_freeBatches.push(std::move(batch));
        m_state.checkCancel();
    }

    // gz / lz4: one output file per input file, each written next to its
    // source (or to the archive path for a single file) via a temporary name.
    void writerLoopPerFile()
    {
        const bool gzip = m_options.format == Format::Gzip;
        OutFile out;
        fs::path temp, target;
        std::uint32_t crc = 0;
        std::uint64_t written = 0;
        auto open = [&](const Item &item, std::size_t index) {
            target = m_outputs[index];
            temp = m_options.discardOutput ? nullDevice() : fs::path(target).concat(".vyra-partial");
            out.create(temp, !m_options.discardOutput && item.size >= (std::uint64_t(16) << 20));
            crc = 0;
            std::uint8_t header[64];
            std::size_t n = 0;
            if (gzip) {
                // Name only when it is plain ASCII: FNAME is ISO-8859-1.
                const std::string name = utf8FromPath(item.source.filename());
                const bool ascii = std::all_of(name.begin(), name.end(), [](char c) { return c > 0 && c < 0x7F; });
                const std::uint32_t unixTime = item.mtime > kFileTimeUnixEpoch ? std::uint32_t((item.mtime - kFileTimeUnixEpoch) / 10000000ULL) : 0;
                header[0] = 0x1F; header[1] = 0x8B; header[2] = 8;
                header[3] = std::uint8_t(0x04 | (ascii ? 0x08 : 0)); // FEXTRA (+ FNAME)
                setLE32(header + 4, unixTime);
                header[8] = 0;  // XFL
                header[9] = 11; // OS: NTFS
                header[10] = 12; header[11] = 0; // XLEN
                header[12] = kGzipSizeId1; header[13] = kGzipSizeId2; header[14] = 8; header[15] = 0;
                for (int k = 0; k < 8; ++k) header[16 + k] = std::uint8_t(item.size >> (8 * k));
                n = 24;
                out.write(header, n);
                if (ascii) out.write(name.c_str(), name.size() + 1);
                n += ascii ? name.size() + 1 : 0;
            } else {
                n = writeLz4SizedFrameHeader(header, lz4BlockMaximum(m_chunkSize), item.size);
                out.write(header, n);
            }
            written = n;
        };
        auto finish = [&](const Item &item) {
            if (gzip) {
                std::uint8_t trailer[8];
                setLE32(trailer, crc);
                setLE32(trailer + 4, std::uint32_t(item.size));
                out.write(trailer, 8);
                written += 8;
            }
            out.close();
            m_state.outputBytes += written;
            if (m_options.discardOutput) return;
            std::error_code ec;
            fs::rename(temp, target, ec);
            if (ec) {
                fs::remove(target, ec);
                fs::rename(temp, target, ec);
                if (ec) throw Error("cannot create " + utf8FromPath(target) + ": " + ec.message());
            }
            applyFileMetadata(target, item.mtime, 0, false);
        };
        // A file still open when the batches stop (cancelled, or failed in
        // another stage) is incomplete: drop its temporary file.
        auto abandon = [&] {
            if (!out.isOpen()) return;
            try { out.close(); } catch (...) {}
            std::error_code ec;
            if (!m_options.discardOutput) fs::remove(temp, ec);
        };
        try {
            std::unique_ptr<Batch> next;
            while (nextBatchInOrder(next)) {
                StageTimer::Scope writeTiming{m_tWrite};
                Batch &b = *next;
                std::size_t c = 0;
                for (std::size_t s = 0; s < b.segments.size(); ++s) {
                    const Segment &seg = b.segments[s];
                    const Item &item = m_items[seg.item];
                    if (seg.first) {
                        m_state.setItem(item.name);
                        open(item, seg.item);
                    }
                    if (item.stream < 0) { // empty file: an empty stream
                        static const std::uint8_t emptyDeflate[2] = {0x03, 0x00}, endMark[4] = {0, 0, 0, 0};
                        if (gzip) out.write(emptyDeflate, 2);
                        else out.write(endMark, 4);
                        written += gzip ? 2 : 4;
                    } else {
                        const std::size_t from = c;
                        while (c < b.chunks.size() && b.chunks[c].offset < seg.offset + seg.size) ++c;
                        writeChunks(b.output, from, c, [&](const std::uint8_t *p, std::size_t n) {
                            out.write(p, n);
                            written += n;
                        });
                        crc = crc32Combine(crc, b.segmentCrc[s], seg.size);
                    }
                    if (seg.last) {
                        finish(item);
                        ++m_state.filesDone;
                    }
                    m_state.bytesDone += seg.size;
                }
                recycleBatch(std::move(next));
            }
            abandon();
        } catch (...) {
            abandon();
            throw;
        }
    }

    void writerLoop()
    {
        if (singleFileFormat(m_options.format)) return writerLoopPerFile();
        std::optional<ZipWriter> zip;
        std::optional<SevenZWriter> sevenZ;
        if (m_options.format == Format::Zip) zip.emplace(m_out);
        else {
            sevenZ.emplace(m_out);
            sevenZ->begin();
        }
        std::vector<std::uint32_t> fileCrc(m_items.size(), 0);
        std::uint32_t entryCrc = 0;
        std::unique_ptr<Batch> next;
        while (nextBatchInOrder(next)) {
            StageTimer::Scope writeTiming{m_tWrite};
            Batch &b = *next;
            std::size_t c = 0;
            for (std::size_t s = 0; s < b.segments.size(); ++s) {
                const Segment &seg = b.segments[s];
                const Item &item = m_items[seg.item];
                if (seg.first) m_state.setItem(item.name);
                if (zip) {
                    if (item.stream < 0) {
                        zip->beginEntry({item.name, item.directory, Method::Store, 0, item.mtime, item.attributes});
                        zip->endEntry(0);
                        ++m_state.filesDone;
                        continue;
                    }
                    if (seg.first) {
                        zip->beginEntry({item.name, false, m_options.method, item.size, item.mtime, item.attributes});
                        entryCrc = 0;
                    }
                    // Compressed chunks are contiguous: one write per run.
                    const std::size_t from = c;
                    while (c < b.chunks.size() && b.chunks[c].offset < seg.offset + seg.size) ++c;
                    writeChunks(b.output, from, c, [&](const std::uint8_t *p, std::size_t n) { zip->writeData(p, n); });
                    entryCrc = crc32Combine(entryCrc, b.segmentCrc[s], seg.size);
                    if (seg.last) {
                        zip->endEntry(entryCrc);
                        ++m_state.filesDone;
                    }
                } else {
                    fileCrc[seg.item] = crc32Combine(fileCrc[seg.item], b.segmentCrc[s], seg.size);
                    if (seg.last) ++m_state.filesDone;
                }
                m_state.bytesDone += seg.size;
                m_state.outputBytes = m_out.tell();
            }
            if (zip) {
                writeChunks(b.output, c, b.chunks.size(), [&](const std::uint8_t *p, std::size_t n) { zip->writeData(p, n); });
            } else {
                // 7z packed streams follow each other: the whole batch output
                // goes out in one write.
                for (std::size_t i = 0; i < b.chunks.size(); ++i) m_folders[b.chunkStream[i]].packSize += b.output.chunkSize(i);
                writeChunks(b.output, 0, b.chunks.size(), [&](const std::uint8_t *p, std::size_t n) { m_out.write(p, n); });
            }
            m_state.outputBytes = m_out.tell();
            recycleBatch(std::move(next));
        }
        if (m_error || m_state.cancel) return;
        m_state.phase = int(Phase::Finalizing);
        if (zip) {
            zip->finish();
        } else {
            std::vector<SevenZFileInfo> files;
            files.reserve(m_items.size());
            for (std::size_t i = 0; i < m_items.size(); ++i) {
                const Item &item = m_items[i];
                SevenZFileInfo f;
                f.name = item.name;
                f.directory = item.directory;
                f.hasStream = item.stream >= 0;
                f.size = item.size;
                f.mtime = item.mtime;
                f.attributes = item.attributes;
                f.crc = fileCrc[i];
                files.push_back(std::move(f));
            }
            sevenZ->finish(files, m_folders);
        }
        m_state.outputBytes = m_out.tell();
    }

    fs::path m_archive;
    CompressOptions m_options;
    SharedState &m_state;
    ThreadPool m_pool;
    std::unique_ptr<Backend> m_backend;
    std::vector<Item> m_items;
    std::vector<fs::path> m_outputs; // gz / lz4: output file per item
    bool m_eachFile = false;         // gz / lz4: outputs next to their sources
    std::map<std::uint64_t, std::unique_ptr<Batch>> m_early; // writer: finished out of order
    std::uint64_t m_expected = 0;
    std::vector<SevenZFolderInfo> m_folders;
    std::size_t m_chunkSize = 65536;
    std::size_t m_batchSize = 0;
    std::size_t m_outputBound = 0; // per batch, from Backend::prepareCompress
    OutFile m_out;
    std::vector<std::unique_ptr<InputBuffer>> m_inputs;
    std::unique_ptr<Backend> m_cpuBackend; // hybrid mode
    int m_hybrid = 0; // 0 off, 1 CPU until the GPU is up, 2 CPU and GPU together, 3 like 2 but the CPU waits for the GPU (tests)
    std::atomic<bool> m_gpuStarted{false};
    std::atomic<bool> m_starterDone{false};
    bool m_timeline = std::getenv("VYRA_PROFILE") && std::string(std::getenv("VYRA_PROFILE")) == "2";
    std::mutex m_inputsMutex;
    bool m_growInputs = false;
    std::size_t m_maxInputs = 0;
    std::atomic<Backend *> m_registerWith{nullptr}; // set once the GPU backend is up
    bool m_mapInputs = false;
    int m_mapShare = 60; // percent of eligible batches that are mapped (measured best 50-67)
    int m_mapCredit = 0;
    int m_mappedBatches = 0; // reader thread only
    std::future<std::pair<std::unique_ptr<Backend>, std::string>> m_gpuInit;
    bool m_wantGpu = false;
    std::size_t m_initialOutput = 0;
    // Reads are usually the bottleneck: their threads (and the reader) run
    // above compression, which takes whatever CPU time is left.
    ThreadPool m_ioPool{ioThreadCount(), true};
    std::vector<Backend *> m_codecs;
    ChunkOutput m_scratch;
    // Pools: bounded by the objects they hold, not by the queue.
    BlockingQueue<int> m_freeBuffers{64};
    BlockingQueue<std::unique_ptr<Batch>> m_freeBatches{64};
    BlockingQueue<std::unique_ptr<Batch>> m_readQueue{64}; // read-ahead is bounded by the input buffers
    BlockingQueue<FileView> m_unmapQueue{1024};
    // Several codec workers finish in bursts; the batch pool bounds these.
    BlockingQueue<std::unique_ptr<Batch>> m_postQueue{64};
    BlockingQueue<std::unique_ptr<Batch>> m_writeQueue{64};
    std::mutex m_errorMutex;
    std::exception_ptr m_error;
    StageTimer m_tRead, m_tReadWait, m_tCrc, m_tCompress, m_tPost, m_tCodecWait, m_tWrite, m_tWriteWait;
};

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

// Serialises access to a backend shared by concurrently decoded folders.
class LockedBackend final : public Backend {
public:
    LockedBackend(Backend &inner, std::mutex &mutex) : m_inner(inner), m_mutex(mutex) {}
    BackendInfo info() const override { return m_inner.info(); }
    bool canCompress(Method m) const override { return m_inner.canCompress(m); }
    bool canDecompress(Method m) const override { return m_inner.canDecompress(m); }
    DeflateShape deflateShape() const override { return m_inner.deflateShape(); }
    void compress(Method method, int level, const std::uint8_t *input, const std::vector<Span> &chunks, ChunkOutput &output) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_inner.compress(method, level, input, chunks, output);
    }
    void decompress(Method method, const std::uint8_t *input, const std::vector<Span> &chunks, std::uint8_t *output,
                    const std::vector<Span> &outputs, std::vector<std::size_t> &actual) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_inner.decompress(method, input, chunks, output, outputs, actual);
    }
    void *allocateHost(std::size_t bytes) override { return m_inner.allocateHost(bytes); }
    void freeHost(void *p) override { m_inner.freeHost(p); }

private:
    Backend &m_inner;
    std::mutex &m_mutex;
};

// ---------------------------------------------------------------------------
// gz / lz4 headers
// ---------------------------------------------------------------------------

struct SingleFileInfo {
    std::string name;            // UTF-8 file name of the content
    std::uint64_t size = 0;
    bool sizeKnown = false;
    std::uint64_t mtime = 0;     // FILETIME, 0 = unknown
    std::uint32_t crc = 0;
    bool hasCrc = false;
    std::uint64_t archiveSize = 0;
};

// Content name from the archive name: "a.txt.gz" -> "a.txt", "a.tgz" -> "a.tar".
std::string contentNameFromArchive(const fs::path &archive)
{
    const std::string ext = lowerAscii(utf8FromPath(archive.extension()));
    std::string stem = utf8FromPath(archive.stem());
    if (stem.empty()) stem = "content";
    if (ext == ".tgz") return stem + ".tar";
    if (ext == ".gz" || ext == ".lz4") return stem;
    return utf8FromPath(archive.filename()) + ".out";
}

SingleFileInfo readSingleFileInfo(const fs::path &archive, Format format)
{
    SingleFileInfo info;
    InFile in(archive);
    info.archiveSize = in.size();
    info.name = contentNameFromArchive(archive);
    std::uint8_t head[16] = {};
    const std::size_t n = in.readSome(head, sizeof(head));
    if (format == Format::Lz4) {
        if (n < 7 || getLE32(head) != 0x184D2204u) throw Error("not an LZ4 file");
        if ((head[4] & 0x08) && n >= 14) {
            info.size = 0;
            for (int k = 7; k >= 0; --k) info.size = (info.size << 8) | head[6 + k];
            info.sizeKnown = true;
        }
        return info;
    }
    if (n < 10 || head[0] != 0x1F || head[1] != 0x8B || head[2] != 8) throw Error("not a gzip file");
    const std::uint8_t flags = head[3];
    const std::uint32_t mtime = getLE32(head + 4);
    if (mtime) info.mtime = std::uint64_t(mtime) * 10000000ULL + kFileTimeUnixEpoch;
    in.seek(10);
    if (flags & 0x04) { // FEXTRA
        std::uint8_t len[2];
        in.readExact(len, 2);
        std::vector<std::uint8_t> extra(len[0] | (len[1] << 8));
        in.readExact(extra.data(), extra.size());
        for (std::size_t p = 0; p + 4 <= extra.size();) {
            const std::size_t sub = extra[p + 2] | (extra[p + 3] << 8);
            if (p + 4 + sub > extra.size()) break;
            if (extra[p] == kGzipSizeId1 && extra[p + 1] == kGzipSizeId2 && sub == 8) {
                info.size = 0;
                for (int k = 7; k >= 0; --k) info.size = (info.size << 8) | extra[p + 4 + k];
                info.sizeKnown = true;
            }
            p += 4 + sub;
        }
    }
    if (flags & 0x08) { // FNAME, ISO-8859-1; keep only the last path component
        std::string latin;
        for (std::uint8_t c = 0; in.readSome(&c, 1) == 1 && c;) latin.push_back(char(c));
        std::string utf8;
        for (unsigned char c : latin) {
            if (c < 0x80) utf8.push_back(char(c));
            else { utf8.push_back(char(0xC0 | (c >> 6))); utf8.push_back(char(0x80 | (c & 0x3F))); }
        }
        const std::size_t slash = utf8.find_last_of("/\\");
        if (slash != std::string::npos) utf8 = utf8.substr(slash + 1);
        // A stored name equal to the archive's own (Python's tarfile writes
        // "x.tgz" into x.tgz) would extract over the archive.
        const bool self = lowerAscii(utf8) == lowerAscii(utf8FromPath(archive.filename()));
        if (!utf8.empty() && utf8 != "." && utf8 != ".." && !self) info.name = utf8;
    }
    // Trailer of the last member: CRC32 and size mod 4 GiB. Exact for files
    // Vyra wrote (one member, size subfield); other files may hold several
    // members, which the decoder checks one by one instead.
    if (info.archiveSize >= 18) {
        std::uint8_t tail[8];
        in.seek(info.archiveSize - 8);
        in.readExact(tail, 8);
        if (info.sizeKnown) {
            info.crc = getLE32(tail);
            info.hasCrc = true;
        } else {
            info.size = getLE32(tail + 4);
        }
    }
    return info;
}

Entry singleFileEntry(const SingleFileInfo &info, Format format)
{
    Entry e;
    e.path = info.name;
    e.size = info.size;
    e.packedSize = info.archiveSize;
    e.mtime = info.mtime;
    e.crc = info.crc;
    e.hasCrc = info.hasCrc;
    e.method = format == Format::Gzip ? Method::Deflate : Method::LZ4;
    return e;
}

bool selected(const std::string &path, const std::vector<std::string> &selection)
{
    if (selection.empty()) return true;
    for (const std::string &s : selection) {
        if (path == s) return true;
        if (path.size() > s.size() && path.compare(0, s.size(), s) == 0 && path[s.size()] == '/') return true;
    }
    return false;
}

// Writes one extracted file: CRC check, size check and metadata.
class FileSink {
public:
    // checkSize: entry.size is exact (false for gzip members of unknown total
    // size); direct: unbuffered writes for big outputs.
    FileSink(const Entry &entry, const fs::path &target, bool write, bool checkSize = true, bool direct = false)
        : m_entry(entry), m_target(target), m_write(write), m_checkSize(checkSize)
    {
        if (m_write) {
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            // Read-only files from an earlier extraction would block the overwrite.
            if (fs::exists(target, ec)) fs::permissions(target, fs::perms::owner_write, fs::perm_options::add, ec);
            m_out.create(target, direct);
        }
    }
    void write(const std::uint8_t *p, std::size_t n)
    {
        m_crc = crc32Update(m_crc, p, n);
        m_size += n;
        if (m_write) m_out.write(p, n);
    }
    void finish()
    {
        if (m_write) m_out.close();
        if (m_checkSize && m_size != m_entry.size) throw Error("size mismatch");
        if (m_entry.hasCrc && m_crc != m_entry.crc) throw Error("CRC mismatch (data is corrupt)");
        if (m_write) applyFileMetadata(m_target, m_entry.mtime, m_entry.attributes, m_entry.hasAttributes);
    }
    void abandon()
    {
        if (!m_write) return;
        try { m_out.close(); } catch (...) {}
    }

private:
    const Entry &m_entry;
    fs::path m_target;
    bool m_write;
    bool m_checkSize;
    OutFile m_out;
    std::uint32_t m_crc = 0;
    std::uint64_t m_size = 0;
};

class Extractor {
public:
    Extractor(const fs::path &archive, const fs::path &destination, const ExtractOptions &options, SharedState &state)
        : m_archive(archive), m_destination(destination), m_options(options), m_state(state), m_pool(options.threads) {}

    OperationResult run(const ProgressCallback &progress)
    {
        OperationResult result;
        m_state.phase = int(m_options.testOnly ? Phase::Testing : Phase::Extracting);
        result.seconds = runWithProgress(m_state, progress, [&] {
            switch (const Format format = detectFormat(m_archive)) {
            case Format::Zip: extractZip(); break;
            case Format::SevenZip: extractSevenZ(); break;
            default: extractSingleFile(format); break;
            }
            finishDirectories();
        });
        result.inputBytes = m_state.bytesDone;
        result.files = m_state.filesDone;
        result.backend = m_state.backend;
        result.warnings = m_warnings;
        return result;
    }

private:
    void warn(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(m_warningMutex);
        m_warnings.push_back(message);
    }

    fs::path target(const std::string &archivePath)
    {
        const fs::path rel = safeRelativePath(archivePath);
        return rel.empty() ? fs::path() : m_destination / rel;
    }

    void makeDirectory(const Entry &e)
    {
        if (m_options.testOnly) return;
        const fs::path dir = target(e.path);
        if (dir.empty()) { warn(e.path + ": unsafe path skipped"); return; }
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) warn(e.path + ": " + ec.message());
        m_directories.push_back({dir, e.mtime});
    }

    // Returns false when the entry must be skipped (unsafe path, keep existing).
    bool prepareFile(const Entry &e, fs::path &out)
    {
        if (m_options.testOnly) return true;
        out = target(e.path);
        if (out.empty()) { warn(e.path + ": unsafe path skipped"); return false; }
        std::error_code ec;
        if (!m_options.overwrite && fs::exists(out, ec)) return false;
        return true;
    }

    void finishDirectories()
    {
        // Deepest first, so setting a child's time does not touch the parent afterwards.
        std::sort(m_directories.begin(), m_directories.end(),
                  [](auto &a, auto &b) { return a.first.native().size() > b.first.native().size(); });
        for (auto &d : m_directories)
            if (d.second) applyFileMetadata(d.first, d.second, 0x10, false);
    }

    void extractZip()
    {
        ZipReader reader;
        reader.open(m_archive);
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.backend = "CPU (" + std::to_string(m_pool.size()) + " threads)";
        }
        std::vector<const ZipEntry *> files;
        for (const ZipEntry &e : reader.entries()) {
            if (!selected(e.path, m_options.selection)) continue;
            if (e.directory) { makeDirectory(e); continue; }
            files.push_back(&e);
            m_state.bytesTotal += e.size;
            ++m_state.filesTotal;
        }
        // Largest first keeps all threads busy until the end.
        std::stable_sort(files.begin(), files.end(), [](auto *a, auto *b) { return a->packedSize > b->packedSize; });
        m_pool.parallelFor(files.size(), [&](std::size_t i) {
            m_state.checkCancel();
            const ZipEntry &e = *files[i];
            try {
                extractZipEntry(reader, e);
            } catch (const Cancelled &) {
                throw;
            } catch (const std::exception &ex) {
                warn(e.path + ": " + ex.what());
            }
            ++m_state.filesDone;
        });
    }

    void extractZipEntry(const ZipReader &reader, const ZipEntry &e)
    {
        if (e.encrypted) throw Error("encrypted entries are not supported");
        if (!e.supported) throw Error(std::string("unsupported compression method ") + std::to_string(e.methodCode));
        fs::path out;
        if (!prepareFile(e, out)) return;
        m_state.setItem(e.path);
        InFile in(reader.path());
        const std::uint64_t offset = reader.dataOffset(in, e);
        in.seek(offset);
        auto decoder = createStreamDecoder({e.method, {}, e.size}, nullptr, 0);
        FileSink sink(e, out, !m_options.testOnly);
        try {
            std::vector<std::uint8_t> buffer(std::size_t(std::min<std::uint64_t>(kReadPiece, std::max<std::uint64_t>(e.packedSize, 1))));
            std::uint64_t remaining = e.packedSize;
            do {
                m_state.checkCancel();
                const std::size_t n = std::size_t(std::min<std::uint64_t>(remaining, buffer.size()));
                in.readExact(buffer.data(), n);
                remaining -= n;
                decoder->feed(buffer.data(), n, remaining == 0, [&](const std::uint8_t *p, std::size_t len) {
                    sink.write(p, len);
                    m_state.bytesDone += len;
                });
            } while (remaining);
            sink.finish();
        } catch (...) {
            sink.abandon();
            throw;
        }
    }

    // gz / lz4: one file, named after the original (gzip FNAME) or after the
    // archive without its extension.
    void extractSingleFile(Format format)
    {
        const SingleFileInfo info = readSingleFileInfo(m_archive, format);
        Entry e = singleFileEntry(info, format);
        if (!selected(e.path, m_options.selection)) return;
        ++m_state.filesTotal;
        m_state.bytesTotal += e.size;
        fs::path out;
        if (!prepareFile(e, out)) {
            ++m_state.filesDone;
            return;
        }
        m_state.setItem(e.path);
        std::uint64_t remaining = info.archiveSize;
        std::unique_ptr<Backend> backend;
        std::unique_ptr<StreamDecoder> decoder;
        if (format == Format::Gzip) {
            decoder = createGzipDecoder(); // checks every member's CRC32 and size
        } else {
            // Independent blocks decode in parallel; the GPU only pays off for big files.
            const BackendKind kind = remaining < (std::uint64_t(256) << 20) ? BackendKind::Cpu : m_options.backend;
            backend = selectBackend(kind, m_pool, m_options.gpuDevice);
            const std::size_t batch = std::size_t(std::min<std::uint64_t>(m_options.batchSize, std::max<std::uint64_t>(remaining, 1 << 16)));
            decoder = createStreamDecoder({Method::LZ4, {}, e.size}, backend.get(), batch);
        }
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.backend = backend ? backend->info().name : "CPU";
        }
        // gzip: the decoder checks every member's CRC32 and size. The header's
        // size and the trailer's CRC describe one member only, which is not the
        // whole content of concatenated files ("cat a.gz b.gz > ab.gz").
        Entry check = e;
        if (format == Format::Gzip) check.hasCrc = false;
        FileSink sink(check, out, !m_options.testOnly, info.sizeKnown && format != Format::Gzip,
                      e.size >= (std::uint64_t(64) << 20));
        InFile in(m_archive);
        std::vector<std::uint8_t> buffer(std::size_t(std::min<std::uint64_t>(kReadPiece, std::max<std::uint64_t>(remaining, 1))));
        try {
            do {
                m_state.checkCancel();
                const std::size_t n = std::size_t(std::min<std::uint64_t>(remaining, buffer.size()));
                in.readExact(buffer.data(), n);
                remaining -= n;
                decoder->feed(buffer.data(), n, remaining == 0, [&](const std::uint8_t *p, std::size_t len) {
                    sink.write(p, len);
                    m_state.bytesDone += len;
                });
            } while (remaining);
            sink.finish();
        } catch (...) {
            sink.abandon();
            throw;
        }
        ++m_state.filesDone;
    }

    void extractSevenZ()
    {
        SevenZReader reader;
        reader.open(m_archive);
        const auto &items = reader.items();
        const auto &folders = reader.folders();
        std::vector<std::vector<std::size_t>> folderItems(folders.size());
        std::vector<bool> wanted(items.size(), false);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const SevenZItem &it = items[i];
            if (it.folder >= 0) folderItems[std::size_t(it.folder)].push_back(i);
            if (!selected(it.path, m_options.selection)) continue;
            wanted[i] = true;
            if (it.directory) { makeDirectory(it); continue; }
            ++m_state.filesTotal;
            m_state.bytesTotal += it.size;
            if (it.folder < 0) {
                fs::path out;
                if (!m_options.testOnly && prepareFile(it, out)) {
                    try {
                        FileSink sink(it, out, true);
                        sink.finish();
                    } catch (const std::exception &ex) {
                        warn(it.path + ": " + ex.what());
                    }
                }
                ++m_state.filesDone;
            }
        }
        // Folders holding at least one wanted file.
        std::vector<std::size_t> small, large;
        bool needBackend = false;
        for (std::size_t f = 0; f < folders.size(); ++f) {
            const bool any = std::any_of(folderItems[f].begin(), folderItems[f].end(), [&](std::size_t i) { return wanted[i]; });
            if (!any) continue;
            const std::uint64_t pack = folders[f].packSizes.empty() ? 0 : folders[f].packSizes[0];
            (pack > m_options.batchSize / 4 ? large : small).push_back(f);
            const auto chain = folders[f].decodeChain();
            if (!chain.empty()) {
                const Method m = folders[f].coders[chain.back()].method;
                needBackend |= m == Method::LZ4 || m == Method::Zstd;
            }
        }
        std::mutex backendMutex;
        std::unique_ptr<Backend> backend;
        std::unique_ptr<LockedBackend> locked;
        if (needBackend) {
            backend = selectBackend(m_options.backend, m_pool, m_options.gpuDevice);
            locked = std::make_unique<LockedBackend>(*backend, backendMutex);
        }
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.backend = backend ? backend->info().name : "CPU (" + std::to_string(m_pool.size()) + " threads)";
        }
        auto decode = [&](std::size_t f, Backend *b) {
            m_state.checkCancel();
            try {
                decodeFolder(reader, f, folderItems[f], wanted, b);
            } catch (const Cancelled &) {
                throw;
            } catch (const std::exception &ex) {
                warn(utf8FromPath(m_archive.filename()) + " block " + std::to_string(f) + ": " + ex.what());
            }
        };
        // Small folders in parallel (non-solid archives), big ones one at a
        // time with the whole backend batch-decoding inside each.
        m_pool.parallelFor(small.size(), [&](std::size_t k) { decode(small[k], locked.get()); });
        for (std::size_t f : large) decode(f, locked.get());
    }

    void decodeFolder(const SevenZReader &reader, std::size_t folderIndex, const std::vector<std::size_t> &members,
                      const std::vector<bool> &wanted, Backend *backend)
    {
        const SevenZFolder &folder = reader.folders()[folderIndex];
        const auto &items = reader.items();
        if (!sevenZFolderSupported(folder)) {
            std::string method = "unknown";
            const auto chain = folder.decodeChain();
            if (!chain.empty()) method = methodName(folder.coders[chain.back()].method);
            for (std::size_t i : members)
                if (wanted[i]) { warn(items[i].path + ": unsupported 7z method (" + method + ")"); ++m_state.filesDone; }
            return;
        }
        const auto chain = folder.decodeChain();
        const SevenZCoder &base = folder.coders[chain.back()];
        const std::size_t batch = std::size_t(std::min<std::uint64_t>(m_options.batchSize, std::max<std::uint64_t>(folder.packSizes[0], 1 << 16)));
        auto decoder = createStreamDecoder({base.method, base.properties, folder.unpackSizes[chain.back()]}, backend, batch);
        for (std::size_t k = chain.size() - 1; k-- > 0;) decoder = createBcjDecoder(std::move(decoder));

        std::size_t member = 0;
        std::uint64_t remainingInItem = 0;
        std::unique_ptr<FileSink> sink;
        bool skipCurrent = false;
        auto finishCurrent = [&] {
            if (member >= members.size()) return;
            const SevenZItem &it = items[members[member]];
            if (sink) {
                try {
                    sink->finish();
                } catch (const std::exception &ex) {
                    warn(it.path + ": " + ex.what());
                }
                sink.reset();
            }
            if (wanted[members[member]]) ++m_state.filesDone;
            ++member;
        };
        auto startItem = [&] {
            while (member < members.size()) {
                const std::size_t idx = members[member];
                const SevenZItem &it = items[idx];
                remainingInItem = it.size;
                skipCurrent = !wanted[idx];
                if (!skipCurrent) {
                    fs::path out;
                    if (prepareFile(it, out)) {
                        m_state.setItem(it.path);
                        sink = std::make_unique<FileSink>(it, out, !m_options.testOnly);
                    } else {
                        skipCurrent = true;
                    }
                }
                if (remainingInItem > 0) return;
                finishCurrent();
            }
        };
        startItem();
        auto consume = [&](const std::uint8_t *p, std::size_t n) {
            while (n) {
                if (member >= members.size()) throw Error("more data than expected in 7z block");
                const std::size_t take = std::size_t(std::min<std::uint64_t>(n, remainingInItem));
                if (sink) sink->write(p, take);
                if (!skipCurrent) m_state.bytesDone += take;
                p += take;
                n -= take;
                remainingInItem -= take;
                if (remainingInItem == 0) {
                    finishCurrent();
                    startItem();
                }
            }
        };
        InFile in(reader.path());
        in.seek(folder.packOffsets[0]);
        std::uint64_t remaining = folder.packSizes[0];
        std::vector<std::uint8_t> buffer(std::size_t(std::min<std::uint64_t>(kReadPiece, std::max<std::uint64_t>(remaining, 1))));
        try {
            do {
                m_state.checkCancel();
                const std::size_t n = std::size_t(std::min<std::uint64_t>(remaining, buffer.size()));
                in.readExact(buffer.data(), n);
                remaining -= n;
                decoder->feed(buffer.data(), n, remaining == 0, consume);
            } while (remaining);
        } catch (...) {
            if (sink) sink->abandon();
            throw;
        }
        if (member < members.size()) {
            if (sink) sink->abandon();
            throw Error("7z block ended early");
        }
    }

    fs::path m_archive;
    fs::path m_destination;
    ExtractOptions m_options;
    SharedState &m_state;
    ThreadPool m_pool;
    std::mutex m_warningMutex;
    std::vector<std::string> m_warnings;
    std::vector<std::pair<fs::path, std::uint64_t>> m_directories;
};

} // namespace

bool methodSupportedFor(Format format, Method method)
{
    switch (format) {
    case Format::Zip: return method == Method::Store || method == Method::Deflate;
    case Format::Gzip: return method == Method::Deflate;
    case Format::Lz4: return method == Method::LZ4;
    default: return method == Method::Store || method == Method::Deflate || method == Method::LZ4 || method == Method::Zstd;
    }
}

fs::path safeRelativePath(const std::string &archivePath)
{
    fs::path out;
    std::size_t start = 0;
    while (start <= archivePath.size()) {
        std::size_t end = archivePath.find_first_of("/\\", start);
        if (end == std::string::npos) end = archivePath.size();
        std::string part = archivePath.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return {};
        // Drive letters, NTFS streams and characters Windows rejects.
        for (char &c : part) {
            if (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' || (unsigned char)c < 32) c = '_';
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '.')) part.pop_back();
        if (part.empty()) part = "_";
        out /= pathFromUtf8(part);
    }
    return out;
}

Format detectFormat(const fs::path &archive)
{
    InFile file(archive);
    std::uint8_t sig[6] = {};
    const std::size_t n = file.readSome(sig, sizeof(sig));
    static const std::uint8_t sevenZ[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    if (n == 6 && std::memcmp(sig, sevenZ, 6) == 0) return Format::SevenZip;
    if (n >= 4 && sig[0] == 'P' && sig[1] == 'K') return Format::Zip;
    if (n >= 3 && sig[0] == 0x1F && sig[1] == 0x8B && sig[2] == 8) return Format::Gzip;
    if (n >= 4 && getLE32(sig) == 0x184D2204u) return Format::Lz4;
    const std::string ext = lowerAscii(utf8FromPath(archive.extension()));
    if (ext == ".7z") return Format::SevenZip;
    if (ext == ".zip" || ext == ".exe") return Format::Zip;
    if (ext == ".gz" || ext == ".tgz") return Format::Gzip;
    if (ext == ".lz4") return Format::Lz4;
    throw Error("unknown archive format: " + utf8FromPath(archive.filename()));
}

ArchiveListing listArchive(const fs::path &archive)
{
    ArchiveListing listing;
    listing.format = detectFormat(archive);
    std::error_code ec;
    listing.archiveSize = fs::file_size(archive, ec);
    std::set<std::string> methods;
    if (singleFileFormat(listing.format)) {
        const SingleFileInfo info = readSingleFileInfo(archive, listing.format);
        listing.entries.push_back(singleFileEntry(info, listing.format));
        methods.insert(methodName(listing.entries.back().method));
    } else if (listing.format == Format::Zip) {
        ZipReader reader;
        reader.open(archive);
        for (const ZipEntry &e : reader.entries()) {
            listing.entries.push_back(e);
            if (!e.directory) methods.insert(e.method == Method::Other ? "method " + std::to_string(e.methodCode) : methodName(e.method));
        }
    } else {
        SevenZReader reader;
        reader.open(archive);
        for (const SevenZItem &it : reader.items()) {
            listing.entries.push_back(it);
            if (it.folder >= 0) methods.insert(methodName(it.method));
        }
        for (const SevenZFolder &f : reader.folders())
            if (f.substreams > 1) listing.solid = true;
    }
    for (const std::string &m : methods) listing.methods += (listing.methods.empty() ? "" : ", ") + m;
    return listing;
}

OperationResult createArchive(const fs::path &archive, const std::vector<fs::path> &inputs,
                              const CompressOptions &options, const ProgressCallback &progress)
{
    if (inputs.empty()) throw Error("nothing to compress");
    SharedState state;
    Compressor compressor(archive, options, state);
    return compressor.run(inputs, progress);
}

OperationResult compressEachFile(const std::vector<fs::path> &inputs, const CompressOptions &options,
                                 const ProgressCallback &progress)
{
    if (inputs.empty()) throw Error("nothing to compress");
    if (!singleFileFormat(options.format)) throw Error("compressEachFile needs the gz or lz4 format");
    SharedState state;
    Compressor compressor(fs::path(), options, state, true);
    return compressor.run(inputs, progress);
}

OperationResult extractArchive(const fs::path &archive, const fs::path &destination, const ExtractOptions &options,
                               const ProgressCallback &progress)
{
    SharedState state;
    Extractor extractor(archive, destination, options, state);
    return extractor.run(progress);
}

} // namespace vyra
