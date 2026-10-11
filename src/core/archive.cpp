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
#include <optional>
#include <set>
#include <thread>

namespace vyra {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kReadPiece = 8u << 20;

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
        output.data.clear();
        output.offsets.clear();
    }
    std::uint64_t sequence = 0;
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

void computeSegmentCrcs(Batch &b, ThreadPool &pool)
{
    struct Piece { std::size_t segment, offset, size; };
    std::vector<Piece> pieces;
    constexpr std::size_t kPiece = 4u << 20;
    for (std::size_t s = 0; s < b.segments.size(); ++s)
        for (std::size_t off = 0; off < b.segments[s].size; off += kPiece)
            pieces.push_back({s, b.segments[s].offset + off, std::min(kPiece, b.segments[s].size - off)});
    std::vector<std::uint32_t> crcs(pieces.size());
    pool.parallelFor(pieces.size(), [&](std::size_t i) { crcs[i] = crc32(b.data + pieces[i].offset, pieces[i].size); });
    b.segmentCrc.assign(b.segments.size(), 0);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        auto &c = b.segmentCrc[pieces[i].segment];
        c = crc32Combine(c, crcs[i], pieces[i].size);
    }
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
    Compressor(const fs::path &archive, const CompressOptions &options, SharedState &state)
        : m_archive(archive), m_options(options), m_state(state), m_pool(options.threads) {}

    OperationResult run(const std::vector<fs::path> &inputs, const ProgressCallback &progress)
    {
        OperationResult result;
        const fs::path temp = fs::path(m_archive).concat(".vyra-partial");
        try {
            result.seconds = runWithProgress(m_state, progress, [&] {
                m_items = scanInputs(inputs, m_archive, m_state, result.warnings);
                prepare();
                m_state.phase = int(Phase::Compressing);
                m_out.create(m_options.discardOutput ? nullDevice() : temp);
                pipeline();
                m_state.phase = int(Phase::Finalizing);
                m_out.close();
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
            if (!m_options.discardOutput) fs::remove(temp, ec);
            throw;
        }
        result.inputBytes = m_state.bytesTotal;
        result.outputBytes = m_state.outputBytes;
        result.files = m_items.size();
        result.backend = m_state.backend;
        return result;
    }

private:
    void prepare()
    {
        const Method method = m_options.method;
        if (!methodSupportedFor(m_options.format, method)) throw Error(std::string(methodName(method)) + " is not available for this format");
        if (method != Method::Store) {
            // nvCOMP's high-ratio Deflate modes are much slower than zlib-ng on
            // a multi-core CPU, so Auto keeps those levels on the CPU.
            const BackendKind kind = m_options.backend == BackendKind::Auto && method == Method::Deflate && m_options.level >= 3
                ? BackendKind::Cpu : m_options.backend;
            m_backend = selectBackend(kind, m_pool, m_options.gpuDevice);
            if (!m_backend->canCompress(method)) m_backend = createCpuBackend(m_pool);
        } else {
            m_backend = createCpuBackend(m_pool);
        }
        // One codec worker per backend. (Two CUDA streams were tried: on WDDM
        // they contend inside the GPU and lose throughput.) Post-processing
        // runs in its own stage so the GPU never waits for the CPU.
        m_codecs = {m_backend.get()};
        const std::string engine = method == Method::Store ? "Store (no compression)" : m_backend->info().name;
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.backend = engine;
        }
        m_chunkSize = std::clamp<std::size_t>(m_options.chunkSize, 4096, method == Method::Deflate ? 65536 : (4u << 20));
        m_chunkSize &= ~std::size_t(255);
        std::size_t batch = std::max<std::size_t>(m_options.batchSize, 4 * m_chunkSize);
        // Never stage more than the input needs (small jobs stay small).
        batch = std::size_t(std::min<std::uint64_t>(batch, m_state.bytesTotal + m_items.size() * 512 + 65536));
        m_batchSize = std::max<std::size_t>(batch, 1 << 16);

        // Assign streams: one per non-empty file (zip) or solid folders (7z).
        int stream = -1;
        std::uint64_t folderBytes = 0;
        for (Item &item : m_items) {
            if (item.directory || item.size == 0) continue;
            if (m_options.format == Format::Zip) {
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
        const int kBuffers = int(m_codecs.size()) + 3, kBatches = kBuffers + 1;
        m_buffers.resize(kBuffers);
        for (int i = 0; i < kBuffers; ++i) {
            m_buffers[i].reset(*m_backend, m_batchSize);
            m_freeBuffers.push(i);
        }
        for (int i = 0; i < kBatches; ++i) {
            auto b = std::make_unique<Batch>();
            b->output.data.setAllocator(m_backend.get());
            m_freeBatches.push(std::move(b));
        }
        std::thread reader([this] {
            try { readerLoop(); } catch (...) { fail(std::current_exception()); }
            m_readQueue.close();
        });
        // Two codec workers, each with its own GPU stream and device buffers,
        // so one batch uploads/downloads while the other is being compressed.
        // The writer restores the order.
        std::atomic<int> codecsRunning{int(m_codecs.size())};
        std::vector<std::thread> codecs;
        for (std::size_t k = 0; k < m_codecs.size(); ++k) {
            codecs.emplace_back([this, k, &codecsRunning] {
                try { codecLoop(*m_codecs[k]); } catch (...) { fail(std::current_exception()); }
                if (--codecsRunning == 0) m_postQueue.close();
            });
        }
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
        for (auto &t : codecs) t.join();
        post.join();
        m_freeBatches.close();
        while (m_freeBatches.pop()) {}
        m_buffers.clear();
        if (std::getenv("VYRA_PROFILE")) {
            std::fprintf(stderr, "[profile] read %.2fs (wait buf %.2fs) | crc %.2fs compress %.2fs post %.2fs (wait input %.2fs) | write %.2fs (wait %.2fs)\n",
                         m_tRead.seconds(), m_tReadWait.seconds(), m_tCrc.seconds(), m_tCompress.seconds(), m_tPost.seconds(),
                         m_tCodecWait.seconds(), m_tWrite.seconds(), m_tWriteWait.seconds());
        }
        if (m_error) std::rethrow_exception(m_error);
        if (m_state.cancel) throw Cancelled();
    }

    std::unique_ptr<Batch> newBatch()
    {
        StageTimer::Scope timing{m_tReadWait};
        auto index = m_freeBuffers.pop();
        if (!index) throw Cancelled();
        auto pooled = m_freeBatches.pop();
        if (!pooled) throw Cancelled();
        auto b = std::move(*pooled);
        b->buffer = *index;
        b->data = m_buffers[std::size_t(*index)].data();
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
            batch->streamStart.push_back(pendingStart ? 1 : 0);
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
            executeReads(*batch);
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
            if (m_options.format == Format::Zip) {
                closeChunk(true);
                current = -1;
            }
        }
        if (current >= 0) closeChunk(true);
        executeReads(*batch);
        if (!m_readQueue.push(std::move(batch))) throw Cancelled();
    }

    void executeReads(Batch &b)
    {
        StageTimer::Scope timing{m_tRead};
        m_ioPool.parallelFor(b.reads.size(), [&](std::size_t k) {
            m_state.checkCancel();
            const ReadTask &t = b.reads[k];
            const Item &item = m_items[t.item];
            if (readFileRange(item.source, t.fileOffset, b.data + t.bufferOffset, t.size) != t.size)
                throw Error("file changed while it was being read: " + utf8FromPath(item.source));
        });
    }

    void codecLoop(Backend &backend)
    {
        const Method method = m_options.method;
        for (;;) {
            std::optional<std::unique_ptr<Batch>> next;
            {
                StageTimer::Scope timing{m_tCodecWait};
                next = m_readQueue.pop();
            }
            if (!next) break;
            Batch &b = **next;
            if (method == Method::Store) {
                computeSegmentCrcs(b, m_pool);
                b.output.offsets.assign(b.chunks.size() + 1, 0);
                for (std::size_t i = 0; i < b.chunks.size(); ++i) b.output.offsets[i + 1] = b.output.offsets[i] + b.chunks[i].size;
                b.output.data.resize(b.output.offsets.back());
                m_pool.parallelFor(b.chunks.size(), [&](std::size_t i) {
                    std::memcpy(b.output.data.data() + b.output.offsets[i], b.data + b.chunks[i].offset, b.chunks[i].size);
                });
            } else if (backend.info().gpu) {
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
                {
                    StageTimer::Scope timing{m_tCrc};
                    computeSegmentCrcs(b, m_pool);
                }
                StageTimer::Scope timing{m_tCompress};
                backend.compress(method, m_options.level, b.data, b.chunks, b.output);
            }
            b.shape = backend.deflateShape();
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
            {
                StageTimer::Scope postTiming{m_tPost};
                if (method == Method::Deflate) joinDeflateChunks(b.output, b.streamEnd, b.shape, m_pool, m_scratch);
                if (method == Method::LZ4) frameLz4(b, lz4BlockMaximum(m_chunkSize), m_pool, m_scratch);
            }
            m_freeBuffers.push(b.buffer);
            b.data = nullptr;
            if (!m_writeQueue.push(std::move(*next))) throw Cancelled();
        }
    }

    void writerLoop()
    {
        std::optional<ZipWriter> zip;
        std::optional<SevenZWriter> sevenZ;
        if (m_options.format == Format::Zip) zip.emplace(m_out);
        else {
            sevenZ.emplace(m_out);
            sevenZ->begin();
        }
        std::vector<std::uint32_t> fileCrc(m_items.size(), 0);
        std::uint32_t entryCrc = 0;
        std::map<std::uint64_t, std::unique_ptr<Batch>> early; // finished out of order
        std::uint64_t expected = 0;
        for (;;) {
            std::optional<std::unique_ptr<Batch>> next;
            auto found = early.find(expected);
            if (found != early.end()) {
                next = std::move(found->second);
                early.erase(found);
            } else {
                {
                    StageTimer::Scope timing{m_tWriteWait};
                    next = m_writeQueue.pop();
                }
                if (!next) {
                    if (!early.empty() && !m_error && !m_state.cancel) throw Error("internal error: missing batch");
                    break;
                }
                if ((*next)->sequence != expected) {
                    const std::uint64_t seq = (*next)->sequence;
                    early.emplace(seq, std::move(*next));
                    continue;
                }
            }
            ++expected;
            StageTimer::Scope writeTiming{m_tWrite};
            Batch &b = **next;
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
                    while (c < b.chunks.size() && b.chunks[c].offset < seg.offset + seg.size) {
                        zip->writeData(b.output.chunk(c), b.output.chunkSize(c));
                        ++c;
                    }
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
                for (; c < b.chunks.size(); ++c) zip->writeData(b.output.chunk(c), b.output.chunkSize(c));
            } else {
                for (std::size_t i = 0; i < b.chunks.size(); ++i) {
                    m_out.write(b.output.chunk(i), b.output.chunkSize(i));
                    m_folders[b.chunkStream[i]].packSize += b.output.chunkSize(i);
                }
            }
            m_state.outputBytes = m_out.tell();
            (*next)->reset();
            m_freeBatches.push(std::move(*next));
            m_state.checkCancel();
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
    std::vector<SevenZFolderInfo> m_folders;
    std::size_t m_chunkSize = 65536;
    std::size_t m_batchSize = 0;
    OutFile m_out;
    std::vector<HostBuffer> m_buffers;
    ThreadPool m_ioPool{std::min(8, defaultThreadCount())};
    std::vector<Backend *> m_codecs;
    ChunkOutput m_scratch;
    BlockingQueue<int> m_freeBuffers{8};
    BlockingQueue<std::unique_ptr<Batch>> m_freeBatches{16};
    BlockingQueue<std::unique_ptr<Batch>> m_readQueue{2};
    BlockingQueue<std::unique_ptr<Batch>> m_postQueue{2};
    BlockingQueue<std::unique_ptr<Batch>> m_writeQueue{2};
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
    FileSink(const Entry &entry, const fs::path &target, bool write) : m_entry(entry), m_target(target), m_write(write)
    {
        if (m_write) {
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            // Read-only files from an earlier extraction would block the overwrite.
            if (fs::exists(target, ec)) fs::permissions(target, fs::perms::owner_write, fs::perm_options::add, ec);
            m_out.create(target);
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
        if (m_size != m_entry.size) throw Error("size mismatch");
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
            if (detectFormat(m_archive) == Format::Zip) extractZip();
            else extractSevenZ();
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
    if (format == Format::Zip) return method == Method::Store || method == Method::Deflate;
    return method == Method::Store || method == Method::Deflate || method == Method::LZ4 || method == Method::Zstd;
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
    const std::string ext = lowerAscii(utf8FromPath(archive.extension()));
    if (ext == ".7z") return Format::SevenZip;
    if (ext == ".zip" || ext == ".exe") return Format::Zip;
    throw Error("unknown archive format: " + utf8FromPath(archive.filename()));
}

ArchiveListing listArchive(const fs::path &archive)
{
    ArchiveListing listing;
    listing.format = detectFormat(archive);
    std::error_code ec;
    listing.archiveSize = fs::file_size(archive, ec);
    std::set<std::string> methods;
    if (listing.format == Format::Zip) {
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

OperationResult extractArchive(const fs::path &archive, const fs::path &destination, const ExtractOptions &options,
                               const ProgressCallback &progress)
{
    SharedState state;
    Extractor extractor(archive, destination, options, state);
    return extractor.run(progress);
}

} // namespace vyra
