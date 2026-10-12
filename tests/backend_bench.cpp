// Backend micro-benchmark: compresses one in-memory batch repeatedly, from one
// or more threads (one backend instance each), then decompresses the chunks
// repeatedly, with no file I/O. GPU times include the PCIe transfers to and
// from page-locked host memory (what the archive pipeline pays), not CUDA
// start-up.
// usage: vyra_backend_bench <file> [threads] [batchMiB]
#include "core/backend.h"
#include "core/threadpool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace vyra;

namespace {
std::size_t packedSize(const ChunkOutput &out)
{
    std::size_t total = 0;
    for (std::size_t i = 0; i < out.count(); ++i) total += out.chunkSize(i);
    return total;
}

// Decompresses chunks made once by `producer` with `backend`, `rounds` times.
void benchDecompress(Method method, Backend &producer, Backend &backend, const char *name, const std::vector<std::uint8_t> &data,
                     const std::vector<Span> &chunks)
{
    HostBuffer source(producer, data.size());
    std::memcpy(source.data(), data.data(), data.size());
    ChunkOutput packed;
    packed.data.setAllocator(&producer);
    producer.compress(method, 1, source.data(), chunks, packed);

    // Compressed chunks back to back in page-locked memory, like the decoder's staging buffer.
    HostBuffer input(backend, packedSize(packed)), output(backend, data.size());
    std::vector<Span> in, out;
    std::size_t at = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        std::memcpy(input.data() + at, packed.chunk(i), packed.chunkSize(i));
        in.push_back({at, packed.chunkSize(i)});
        out.push_back({chunks[i].offset, chunks[i].size});
        at += packed.chunkSize(i);
    }
    std::vector<std::size_t> actual;
    backend.decompress(method, input.data(), in, output.data(), out, actual); // warm-up
    const bool correct = std::memcmp(output.data(), data.data(), data.size()) == 0;
    const int rounds = 8;
    const auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; ++r) backend.decompress(method, input.data(), in, output.data(), out, actual);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%-4s decompress %-8s %7.2f GiB/s%s\n", name, methodName(method), double(data.size()) * rounds / s / double(1u << 30),
                correct ? "" : "  OUTPUT DIFFERS");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: %s <file> [threads] [batchMiB]\n", argv[0]); return 1; }
    const int threads = argc > 2 ? std::atoi(argv[2]) : 1;
    const std::size_t batch = (argc > 3 ? std::size_t(std::atoi(argv[3])) : 128) << 20;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> data(batch);
    f.read(reinterpret_cast<char *>(data.data()), std::streamsize(batch));
    data.resize(std::size_t(f.gcount()));
    std::vector<Span> chunks;
    for (std::size_t off = 0; off < data.size(); off += 65536) chunks.push_back({off, std::min<std::size_t>(65536, data.size() - off)});

    ThreadPool pool;
    for (Method method : {Method::LZ4, Method::Zstd, Method::Deflate}) {
        for (bool gpu : {true, false}) {
            std::vector<std::unique_ptr<Backend>> backends;
            for (int t = 0; t < (gpu ? threads : 1); ++t) {
                auto b = gpu ? createGpuBackend(0) : createCpuBackend(pool);
                if (!b) break;
                backends.push_back(std::move(b));
            }
            if (backends.empty()) continue;
            // Pinned input per backend, like the real pipeline.
            std::vector<HostBuffer> inputs;
            for (auto &b : backends) {
                inputs.emplace_back(*b, data.size());
                std::copy(data.begin(), data.end(), inputs.back().data());
            }
            const int rounds = 8;
            std::atomic<std::uint64_t> outBytes{0};
            auto worker = [&](std::size_t k, bool measure) {
                ChunkOutput out;
                out.data.setAllocator(backends[k].get());
                for (int r = 0; r < rounds; ++r) {
                    backends[k]->compress(method, 1, inputs[k].data(), chunks, out);
                    if (measure && r == 0) outBytes += packedSize(out);
                }
            };
            worker(0, false); // warm-up
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::thread> ts;
            for (std::size_t k = 0; k < backends.size(); ++k) ts.emplace_back(worker, k, true);
            for (auto &t : ts) t.join();
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double total = double(data.size()) * rounds * double(backends.size());
            std::printf("%-4s x%zu %-8s %7.2f GiB/s  ratio %.1f%%\n", gpu ? "GPU" : "CPU", backends.size(), methodName(method),
                        total / s / double(1u << 30), 100.0 * double(outBytes) / double(data.size()) / double(backends.size()));
        }
    }

    // Decompression of the same data. The GPU's chunks serve both decoders
    // (complete streams; CPU Deflate chunks are sync-flushed, not final).
    auto gpu = createGpuBackend(0);
    auto cpu = createCpuBackend(pool);
    for (Method method : {Method::LZ4, Method::Zstd, Method::Deflate}) {
        Backend &producer = gpu ? *gpu : *cpu;
        if (gpu) benchDecompress(method, producer, *gpu, "GPU", data, chunks);
        benchDecompress(method, producer, *cpu, "CPU", data, chunks);
    }
    return 0;
}
