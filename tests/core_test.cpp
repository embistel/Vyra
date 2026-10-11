// Core round-trip tests: every format x method x backend, plus the Deflate
// joiner, path safety and cancellation. Usage: vyra_tests <work-dir>
#include "core/archive.h"
#include "core/backend.h"
#include "core/deflatestream.h"
#include "core/fileio.h"
#include "core/streamdecoder.h"
#include "core/threadpool.h"

#include <cstdio>
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

void testRoundTrip(const fs::path &work, const fs::path &fixture, Format format, Method method, BackendKind backend, std::size_t batch)
{
    const std::string name = std::string(formatName(format)) + "-" + methodName(method) + (backend == BackendKind::Gpu ? "-gpu" : "-cpu") +
                             "-b" + std::to_string(batch >> 10);
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
        const bool same = snapshot(fixture) == snapshot(out / fixture.filename());
        if (!same) std::cerr << "  mismatch in " << name << "\n";
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
    if (gpu) testDeflateJoin(*gpu, pool);
    else std::cout << "GPU tests skipped: " << reason << "\n";

    std::vector<BackendKind> backends = {BackendKind::Cpu};
    if (gpu) backends.push_back(BackendKind::Gpu);
    gpu.reset();
    for (BackendKind b : backends) {
        for (std::size_t batch : {std::size_t(128u << 20), std::size_t(256u << 10)}) {
            testRoundTrip(work, fixture, Format::Zip, Method::Deflate, b, batch);
            testRoundTrip(work, fixture, Format::Zip, Method::Store, b, batch);
            testRoundTrip(work, fixture, Format::SevenZip, Method::LZ4, b, batch);
            testRoundTrip(work, fixture, Format::SevenZip, Method::Zstd, b, batch);
            testRoundTrip(work, fixture, Format::SevenZip, Method::Deflate, b, batch);
            testRoundTrip(work, fixture, Format::SevenZip, Method::Store, b, batch);
        }
    }
    testSelectionAndCancel(work, fixture);
    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all core tests passed\n";
    return 0;
}
