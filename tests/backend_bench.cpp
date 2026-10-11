// Backend micro-benchmark: compresses one in-memory batch repeatedly, from one
// or more threads (one backend instance each), with no file I/O.
// usage: vyra_backend_bench <file> [threads] [batchMiB]
#include "core/backend.h"
#include "core/threadpool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace vyra;

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
            auto worker = [&](std::size_t k) {
                ChunkOutput out;
                out.data.setAllocator(backends[k].get());
                for (int r = 0; r < rounds; ++r) {
                    backends[k]->compress(method, 1, inputs[k].data(), chunks, out);
                    if (r == 0) outBytes += out.data.size();
                }
            };
            worker(0); // warm-up
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::thread> ts;
            for (std::size_t k = 0; k < backends.size(); ++k) ts.emplace_back(worker, k);
            for (auto &t : ts) t.join();
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double total = double(data.size()) * rounds * double(backends.size());
            std::printf("%-4s x%zu %-8s %7.2f GiB/s  ratio %.1f%%\n", gpu ? "GPU" : "CPU", backends.size(), methodName(method),
                        total / s / double(1u << 30), 100.0 * double(outBytes) / double(data.size()) / double(backends.size()) * 1.0);
        }
    }
    return 0;
}
