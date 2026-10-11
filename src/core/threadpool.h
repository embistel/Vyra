#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace vyra {

// Small fixed-size pool. parallelFor() may be called concurrently from several
// threads; the calling thread also works on its own loop, so nested or
// concurrent use cannot deadlock.
class ThreadPool {
public:
    explicit ThreadPool(int threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    int size() const { return int(m_workers.size()) + 1; }

    // Runs fn(i) for i in [0, count). Rethrows the first exception thrown.
    void parallelFor(std::size_t count, const std::function<void(std::size_t)> &fn);

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    struct Task {
        const void *owner;
        std::function<void()> fn;
    };
    std::deque<Task> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
};

int defaultThreadCount();

} // namespace vyra
