// Core round-trip tests: every format x method x backend (gz / lz4: one
// output per file), plus the Deflate joiner, path safety and cancellation.
// Usage: vyra_tests <work-dir>
#include "core/archive.h"
#include "core/backend.h"
#include "core/crc32.h"
#include "core/deflatestream.h"
#include "core/fileio.h"
#include "core/streamdecoder.h"
#include "core/threadpool.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace vyra;

namespace {
int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; } } while (0)

std::vector<std::uint8_t> textLike(std::size_t size, unsigned seed)
{
    static const char *words[] = {"vyra ", "gpu ", "archive ", "nvcomp ", "deflate ", "lz4 ", "zstd ", "7z ", "zip ", "\n"};
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out;
    out.reserve(size);
    while (out.size() < size) {
        const char *w = words[rng() % 10];
        out.insert(out.end(), w, w + std::strlen(w));
        if (rng() % 17 == 0) out.push_back(std::uint8_t(rng()));
    }
    out.resize(size);
    return out;
}

std::vector<std::uint8_t> randomBytes(std::size_t size, unsigned seed)
{
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out(size);
    for (auto &b : out) b = std::uint8_t(rng());
    return out;
}

void writeFile(const fs::path &path, const std::vector<std::uint8_t> &data)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
}

std::vector<std::uint8_t> readFile(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

// Snapshot of a tree: relative path -> content ("<dir>" for directories).
std::map<std::string, std::vector<std::uint8_t>> snapshot(const fs::path &root)
{
    std::map<std::string, std::vector<std::uint8_t>> out;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        const std::string rel = it->path().lexically_relative(root).generic_u8string();
        if (it->is_directory()) out[rel] = {'<', 'd', 'i', 'r', '>'};
        else out[rel] = readFile(it->path());
    }
    return out;
}

void buildFixture(const fs::path &root)
{
    fs::remove_all(root);
    writeFile(root / "text.txt", textLike(300000, 1));
    writeFile(root / "random.bin", randomBytes(200000, 2));
    writeFile(root / "empty.txt", {});
    writeFile(root / "sub" / "exact64k.bin", textLike(65536, 3));
    writeFile(root / "sub" / "multi.bin", textLike(65536 * 5, 4));
    writeFile(root / "sub" / "deeper" / "big.txt", textLike(3 * 1024 * 1024 + 123, 5));
    writeFile(root / fs::u8path(u8"한글 폴더") / fs::u8path(u8"파일 이름.txt"), textLike(5000, 6));
    fs::create_directories(root / "empty dir");
    for (int i = 0; i < 40; ++i) writeFile(root / "many" / ("f" + std::to_string(i) + ".txt"), textLike(std::size_t(i) * 37, 100 + unsigned(i)));
}

void testDeflateJoin(Backend &backend, ThreadPool &pool)
{
    const auto data = textLike(1 << 20, 7);
    std::vector<Span> chunks;
    for (std::size_t off = 0; off < data.size(); off += 65536) chunks.push_back({off, std::min<std::size_t>(65536, data.size() - off)});
    ChunkOutput out, scratch;
    backend.compress(Method::Deflate, 1, data.data(), chunks, out);
    std::vector<std::uint8_t> ends(chunks.size(), 0);
    ends.back() = 1;
    joinDeflateChunks(out, ends, backend.deflateShape(), pool, scratch);
    auto decoder = createStreamDecoder({Method::Deflate, {}, data.size()}, nullptr, 0);
    std::vector<std::uint8_t> decoded;
    decoder->feed(out.data.data(), out.data.size(), true, [&](const std::uint8_t *p, std::size_t n) { decoded.insert(decoded.end(), p, p + n); });
    CHECK(decoded == data);
}

// Device-side framing (GPU compressFramed): several streams of varied data in
// one batch, each decoded on its own. Run with VYRA_VERIFY_DEFLATE_SCAN set
// (main does), so every fast Deflate end-of-block result is cross-checked by
// the full Huffman walk.
void testFramed(Backend &backend)
{
    std::vector<std::vector<std::uint8_t>> streams;
    streams.push_back(textLike(5 * 1024 * 1024, 11));
    streams.push_back(randomBytes(3 * 1024 * 1024 + 123, 12));
    streams.push_back(textLike(1, 13));
    streams.push_back(textLike(65536, 14));
    std::vector<std::uint8_t> dna(7 * 1024 * 1024);
    std::mt19937 rng(15);
    for (auto &b : dna) b = std::uint8_t("ACGT"[rng() % 4]);
    streams.push_back(dna);
    streams.push_back(std::vector<std::uint8_t>(2 * 1024 * 1024 + 77, 0));
    std::vector<std::uint8_t> input;
    std::vector<Span> chunks;
    std::vector<std::uint8_t> starts, ends;
    std::vector<std::size_t> firstChunk;
    for (const auto &st : streams) {
        input.resize((input.size() + 255) & ~std::size_t(255));
        const std::size_t base = input.size();
        input.insert(input.end(), st.begin(), st.end());
        firstChunk.push_back(chunks.size());
        for (std::size_t off = 0; off < st.size(); off += 65536) {
            chunks.push_back({base + off, std::min<std::size_t>(65536, st.size() - off)});
            starts.push_back(off == 0);
            ends.push_back(off + 65536 >= st.size());
        }
    }
    firstChunk.push_back(chunks.size());
    for (Method method : {Method::Deflate, Method::LZ4, Method::Zstd}) {
        CompressExtras extras;
        extras.streamStart = &starts;
        extras.streamEnd = &ends;
        extras.lz4BlockMaximum = 65536;
        std::vector<Span> ranges;
        for (std::size_t k = 0; k < streams.size(); ++k)
            ranges.push_back({chunks[firstChunk[k]].offset, streams[k].size()});
        std::vector<std::uint32_t> crcs;
        extras.crcRanges = &ranges;
        extras.crcs = &crcs;
        ChunkOutput out;
        try {
            CHECK(backend.compressFramed(method, 1, input.data(), chunks, extras, out));
        } catch (const std::exception &e) {
            ++g_failures;
            std::cerr << "FAIL framed " << methodName(method) << ": " << e.what() << "\n";
            continue;
        }
        CHECK(crcs.size() == streams.size());
        for (std::size_t k = 0; k < streams.size(); ++k) {
            const std::size_t from = out.offsets[firstChunk[k]], to = out.offsets[firstChunk[k + 1]];
            auto decoder = createStreamDecoder({method, {}, streams[k].size()}, nullptr, 0);
            std::vector<std::uint8_t> decoded;
            decoder->feed(out.data.data() + from, to - from, true,
                          [&](const std::uint8_t *p, std::size_t n) { decoded.insert(decoded.end(), p, p + n); });
            if (decoded != streams[k]) std::cerr << "  framed " << methodName(method) << " stream " << k << " differs\n";
            CHECK(decoded == streams[k]);
            if (k < crcs.size()) CHECK(crcs[k] == crc32(streams[k].data(), streams[k].size()));
        }
        std::cout << "ok   framed " << methodName(method) << "\n";
    }
}

void setEnv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void testRoundTrip(const fs::path &work, const fs::path &fixture, Format format, Method method, BackendKind backend, std::size_t batch,
                   const std::string &tag)
{
    const std::string name = std::string(formatName(format)) + "-" + methodName(method) + "-" + tag + "-b" + std::to_string(batch >> 10);
    const fs::path archive = work / (name + (format == Format::Zip ? ".zip" : ".7z"));
    const fs::path out = work / (name + "-out");
    fs::remove_all(out);
    CompressOptions o;
    o.format = format;
    o.method = method;
    o.backend = backend;
    o.batchSize = batch;
    o.solidBlockSize = format == Format::SevenZip && batch < (1u << 20) ? (1u << 20) : (4ull << 30); // also exercise several folders
    try {
        createArchive(archive, {fixture}, o);
        ExtractOptions x;
        x.backend = backend;
        x.batchSize = batch;
        const OperationResult r = extractArchive(archive, out, x);
        CHECK(r.warnings.empty());
        for (const auto &w : r.warnings) std::cerr << "  warning: " << w << "\n";
        const auto expected = snapshot(fixture), actual = snapshot(out / fixture.filename());
        const bool same = expected == actual;
        if (!same) {
            std::cerr << "  mismatch in " << name << "\n";
            for (const auto &[path, data] : expected) {
                auto it = actual.find(path);
                if (it == actual.end()) { std::cerr << "    missing " << path << "\n"; continue; }
                if (it->second == data) continue;
                std::size_t at = 0;
                while (at < data.size() && at < it->second.size() && data[at] == it->second[at]) ++at;
                std::cerr << "    differs " << path << ": size " << data.size() << " vs " << it->second.size() << ", first difference at " << at << "\n";
            }
            for (const auto &[path, data] : actual)
                if (!expected.count(path)) std::cerr << "    extra " << path << "\n";
        }
        CHECK(same);
        ExtractOptions t;
        t.testOnly = true;
        t.backend = backend;
        CHECK(extractArchive(archive, {}, t).warnings.empty());
        std::cout << "ok   " << name << "\n";
    } catch (const std::exception &e) {
        ++g_failures;
        std::cerr << "FAIL " << name << ": " << e.what() << "\n";
    }
}

// Extracts a gz / lz4 file into `dest` and returns the one file it holds.
std::vector<std::uint8_t> extractSingle(const fs::path &packed, const fs::path &dest, BackendKind backend, std::string *name = nullptr)
{
    fs::remove_all(dest);
    ExtractOptions x;
    x.backend = backend;
    const OperationResult r = extractArchive(packed, dest, x);
    CHECK(r.warnings.empty());
    CHECK(r.files == 1);
    for (const auto &w : r.warnings) std::cerr << "  warning: " << w << "\n";
    for (const auto &entry : fs::directory_iterator(dest)) {
        if (name) *name = entry.path().filename().u8string();
        return readFile(entry.path());
    }
    return {};
}

// gz / lz4: compressEachFile writes <file>.gz / <file>.lz4 next to every file
// (originals kept; an existing output and an already compressed file are
// skipped); each output decodes back to its file, under its name. Then one
// explicit output (createArchive) and its one-file rule.
void testEachFile(const fs::path &work, const fs::path &fixture, Format format, BackendKind backend, std::size_t batch,
                  const std::string &tag)
{
    const std::string ext = std::string(".") + formatName(format);
    const std::string name = std::string(formatName(format)) + "-each-" + tag + "-b" + std::to_string(batch >> 10);
    const fs::path tree = work / (name + "-tree");
    fs::remove_all(tree);
    fs::copy(fixture, tree, fs::copy_options::recursive);
    const std::string conflict = "sub/exact64k.bin";
    const bool hasConflict = fs::exists(tree / conflict);
    if (hasConflict) writeFile(tree / fs::u8path(conflict + ext), {'k', 'e', 'e', 'p'});
    const auto before = snapshot(tree);
    CompressOptions o;
    o.format = format;
    o.method = format == Format::Gzip ? Method::Deflate : Method::LZ4;
    o.backend = backend;
    o.batchSize = batch;
    try {
        const OperationResult r = compressEachFile({tree}, o);
        CHECK(r.warnings.size() == (hasConflict ? 2u : 0u));
        for (const auto &w : r.warnings)
            if (!hasConflict) std::cerr << "  warning: " << w << "\n";
        const auto after = snapshot(tree);
        std::uint64_t outputs = 0;
        for (const auto &[path, data] : before) {
            const auto kept = after.find(path);
            CHECK(kept != after.end() && kept->second == data); // originals stay
            const bool directory = data == std::vector<std::uint8_t>{'<', 'd', 'i', 'r', '>'};
            if (directory || (path.size() > ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0)) continue;
            const fs::path packed = tree / fs::u8path(path + ext);
            if (hasConflict && path == conflict) {
                CHECK(readFile(packed) == std::vector<std::uint8_t>({'k', 'e', 'e', 'p'}));
                continue;
            }
            ++outputs;
            if (!fs::exists(packed)) {
                ++g_failures;
                std::cerr << "FAIL " << name << ": no output for " << path << "\n";
                continue;
            }
            const ArchiveListing listing = listArchive(packed);
            CHECK(listing.format == format && listing.entries.size() == 1);
            if (!listing.entries.empty()) CHECK(listing.entries[0].size == data.size());
            std::string extracted;
            const auto decoded = extractSingle(packed, work / (name + "-out"), backend, &extracted);
            if (decoded != data) std::cerr << "  " << name << ": " << path << " differs\n";
            CHECK(decoded == data);
            CHECK(extracted == fs::u8path(path).filename().u8string());
            ExtractOptions t;
            t.testOnly = true;
            CHECK(extractArchive(packed, {}, t).warnings.empty());
        }
        CHECK(r.files == outputs);
        CHECK(after.size() == before.size() + outputs);

        // One explicit output; more than one file is refused.
        const fs::path single = work / (name + "-single" + ext);
        fs::path source;
        for (const auto &[path, data] : before)
            if (data.size() > 1000 && path.find(ext) == std::string::npos) source = tree / fs::u8path(path);
        createArchive(single, {source}, o);
        CHECK(extractSingle(single, work / (name + "-out"), backend) == readFile(source));
        const fs::path two = work / (name + "-two" + ext);
        bool refused = false;
        try {
            createArchive(two, {tree}, o);
        } catch (const std::exception &) {
            refused = true;
        }
        CHECK(refused || before.size() < 2);
        CHECK(!fs::exists(two));
        std::cout << "ok   " << name << "\n";
    } catch (const std::exception &e) {
        ++g_failures;
        std::cerr << "FAIL " << name << ": " << e.what() << "\n";
    }
}

// Concatenated gzip files ("cat a.gz b.gz > ab.gz", trailing zeros) hold
// the contents one after the other.
void testGzipConcatenation(const fs::path &work, const fs::path &fixture)
{
    CompressOptions o;
    o.format = Format::Gzip;
    o.method = Method::Deflate;
    o.backend = BackendKind::Cpu;
    const fs::path a = work / "concat-a.gz", b = work / "concat-b.gz", ab = work / "concat-ab.gz";
    try {
        createArchive(a, {fixture / "text.txt"}, o);
        createArchive(b, {fixture / "sub" / "multi.bin"}, o);
        auto joined = readFile(a);
        const auto second = readFile(b);
        joined.insert(joined.end(), second.begin(), second.end());
        joined.insert(joined.end(), 512, 0);
        writeFile(ab, joined);
        auto expected = readFile(fixture / "text.txt");
        const auto tail = readFile(fixture / "sub" / "multi.bin");
        expected.insert(expected.end(), tail.begin(), tail.end());
        CHECK(extractSingle(ab, work / "concat-out", BackendKind::Cpu) == expected);
        // A damaged member is reported.
        joined[joined.size() - 512 - 6] ^= 0x55; // inside the second member's CRC32
        writeFile(ab, joined);
        bool failed = false;
        try {
            ExtractOptions t;
            t.testOnly = true;
            failed = !extractArchive(ab, {}, t).warnings.empty();
        } catch (const std::exception &) {
            failed = true;
        }
        CHECK(failed);
        std::cout << "ok   gz concatenation\n";
    } catch (const std::exception &e) {
        ++g_failures;
        std::cerr << "FAIL gz concatenation: " << e.what() << "\n";
    }
}

// Cancelling gz / lz4 while files are written leaves complete outputs only:
// files finished before the cancel stay (like gzip), the one in progress
// leaves no partial file. Where a cancel lands is timing dependent, so
// several cancel points are tried.
void testEachFileCancel(const fs::path &work)
{
    const fs::path tree = work / "cancel-each";
    fs::remove_all(tree);
    fs::create_directories(tree);
    const auto block = randomBytes(1u << 20, 31); // repeats beyond any match window
    for (int k = 0; k < 3; ++k) {
        std::ofstream f(tree / ("part" + std::to_string(k) + ".bin"), std::ios::binary);
        for (int i = 0; i < 32; ++i) f.write(reinterpret_cast<const char *>(block.data()), std::streamsize(block.size()));
    }
    for (Format format : {Format::Gzip, Format::Lz4}) {
        const std::string ext = std::string(".") + formatName(format);
        int cancelledRuns = 0, kept = 0;
        for (int point = 1; point <= 3; ++point) {
            for (int k = 0; k < 3; ++k) fs::remove(tree / ("part" + std::to_string(k) + ".bin" + ext));
            CompressOptions o;
            o.format = format;
            o.method = format == Format::Gzip ? Method::Deflate : Method::LZ4;
            o.backend = BackendKind::Cpu;
            o.threads = 2; // slow enough for progress ticks to land inside the job
            o.batchSize = 4u << 20;
            try {
                compressEachFile({tree}, o, [point](const Progress &p) {
                    return p.bytesTotal == 0 || p.bytesDone * 4 < p.bytesTotal * std::uint64_t(point);
                });
            } catch (const Cancelled &) {
                ++cancelledRuns;
            } catch (const std::exception &e) {
                ++g_failures;
                std::cerr << "FAIL cancel " << formatName(format) << ": " << e.what() << "\n";
            }
            for (const auto &entry : fs::directory_iterator(tree)) {
                const std::string name = entry.path().filename().u8string();
                if (name.find(".vyra-partial") != std::string::npos) {
                    ++g_failures;
                    std::cerr << "FAIL cancel " << formatName(format) << ": partial output " << name << " left\n";
                }
            }
            for (int k = 0; k < 3; ++k) {
                const fs::path packed = tree / ("part" + std::to_string(k) + ".bin" + ext);
                if (!fs::exists(packed)) continue;
                ++kept;
                CHECK(extractSingle(packed, work / "cancel-out", BackendKind::Cpu) == readFile(tree / ("part" + std::to_string(k) + ".bin")));
            }
        }
        std::cout << "ok   " << formatName(format) << " cancel (" << cancelledRuns << "/3 cancelled, " << kept
                  << " complete outputs kept, no partial file)\n";
    }
}

void testSelectionAndCancel(const fs::path &work, const fs::path &fixture)
{
    const fs::path archive = work / "select.7z";
    CompressOptions o;
    o.backend = BackendKind::Cpu;
    createArchive(archive, {fixture}, o);
    const fs::path out = work / "select-out";
    fs::remove_all(out);
    ExtractOptions x;
    x.backend = BackendKind::Cpu;
    x.selection = {fixture.filename().u8string() + "/sub"};
    extractArchive(archive, out, x);
    CHECK(fs::exists(out / fixture.filename() / "sub" / "multi.bin"));
    CHECK(!fs::exists(out / fixture.filename() / "text.txt"));

    const fs::path cancelled = work / "cancelled.zip";
    bool threw = false;
    try {
        CompressOptions c;
        c.format = Format::Zip;
        c.method = Method::Deflate;
        c.backend = BackendKind::Cpu;
        c.batchSize = 1 << 16;
        createArchive(cancelled, {fixture}, c, [](const Progress &) { return false; });
    } catch (const Cancelled &) {
        threw = true;
    } catch (const std::exception &) {
    }
    // A job may finish before the first progress tick; either way no partial file stays.
    CHECK(threw || fs::exists(cancelled));
    CHECK(!fs::exists(fs::path(cancelled).concat(".vyra-partial")));
}

void testSafePaths()
{
    CHECK(safeRelativePath("../evil.txt").empty());
    CHECK(safeRelativePath("a/../../b").empty());
    CHECK(safeRelativePath("/abs/x.txt") == fs::path("abs") / "x.txt");
    CHECK(safeRelativePath("C:/x.txt") == fs::path("C_") / "x.txt");
    CHECK(safeRelativePath("a\\b\\c") == fs::path("a") / "b" / "c");
}
} // namespace

int main(int argc, char **argv)
{
    setEnv("VYRA_VERIFY_DEFLATE_SCAN", "1"); // before the GPU backend is created
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "vyra-tests";
    fs::create_directories(work);
    const fs::path fixture = work / "fixture";
    buildFixture(fixture);
    testSafePaths();

    ThreadPool pool;
    auto cpu = createCpuBackend(pool);
    testDeflateJoin(*cpu, pool);
    std::string reason;
    auto gpu = createGpuBackend(0, &reason);
    if (gpu) {
        testDeflateJoin(*gpu, pool);
        testFramed(*gpu);
    }
    else std::cout << "GPU tests skipped: " << reason << "\n";

    // CPU only; GPU only (no CPU worker); GPU + CPU worker (hybrid, the
    // default with a GPU, where both engines' output mixes in one stream).
    struct Pass { BackendKind backend; const char *hybrid; const char *tag; };
    std::vector<Pass> passes = {{BackendKind::Cpu, "2", "cpu"}};
    if (gpu) {
        passes.push_back({BackendKind::Gpu, "0", "gpu"});
        passes.push_back({BackendKind::Gpu, "3", "mixed"}); // CPU and GPU batches in one stream
    }
    gpu.reset();
    for (const Pass &p : passes) {
        setEnv("VYRA_HYBRID", p.hybrid);
        for (std::size_t batch : {std::size_t(128u << 20), std::size_t(256u << 10)}) {
            testRoundTrip(work, fixture, Format::Zip, Method::Deflate, p.backend, batch, p.tag);
            testRoundTrip(work, fixture, Format::Zip, Method::Store, p.backend, batch, p.tag);
            testRoundTrip(work, fixture, Format::SevenZip, Method::LZ4, p.backend, batch, p.tag);
            testRoundTrip(work, fixture, Format::SevenZip, Method::Zstd, p.backend, batch, p.tag);
            testRoundTrip(work, fixture, Format::SevenZip, Method::Deflate, p.backend, batch, p.tag);
            testRoundTrip(work, fixture, Format::SevenZip, Method::Store, p.backend, batch, p.tag);
            testEachFile(work, fixture, Format::Gzip, p.backend, batch, p.tag);
            testEachFile(work, fixture, Format::Lz4, p.backend, batch, p.tag);
        }
    }
    // Big single file, small CPU batches: exercises the mapped-input path
    // (batches of one file >= 4 MiB, mixed with copied ones), asynchronous
    // unmapping and multi-block direct writes (archives > 16 MiB).
    const fs::path large = work / "large";
    fs::remove_all(large);
    {
        auto data = textLike(32u << 20, 21);
        const auto noise = randomBytes(16u << 20, 22);
        data.insert(data.end(), noise.begin(), noise.end());
        writeFile(large / "big48.bin", data);
    }
    setEnv("VYRA_HYBRID", "2");
    for (Method method : {Method::LZ4, Method::Zstd, Method::Deflate})
        testRoundTrip(work, large, Format::SevenZip, method, BackendKind::Cpu, std::size_t(8) << 20, "cpu-mapped");
    testRoundTrip(work, large, Format::Zip, Method::Deflate, BackendKind::Cpu, std::size_t(8) << 20, "cpu-mapped");
    testEachFile(work, large, Format::Gzip, BackendKind::Cpu, std::size_t(8) << 20, "cpu-mapped");
    testEachFile(work, large, Format::Lz4, BackendKind::Cpu, std::size_t(8) << 20, "cpu-mapped");
    testGzipConcatenation(work, fixture);
    testEachFileCancel(work);

    testSelectionAndCancel(work, fixture);
    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all core tests passed\n";
    return 0;
}
