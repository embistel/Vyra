#include "core/threadpool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>

namespace vyra {

int defaultThreadCount()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n ? int(std::min(n, 64u)) : 4;
}

ThreadPool::ThreadPool(int threads)
{
    if (threads <= 0) threads = defaultThreadCount();
    for (int i = 1; i < threads; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (auto &worker : m_workers) worker.join();
}

void ThreadPool::workerLoop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
            if (m_stop && m_tasks.empty()) return;
            task = std::move(m_tasks.front().fn);
            m_tasks.pop_front();
        }
        task();
    }
}

void ThreadPool::parallelFor(std::size_t count, const std::function<void(std::size_t)> &fn)
{
    if (count == 0) return;
    if (count == 1 || m_workers.empty()) {
        for (std::size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    struct Shared {
        std::atomic<std::size_t> next{0};
        std::atomic<bool> failed{false};
        std::exception_ptr error;
        std::mutex mutex;
        std::condition_variable done;
        int active = 0;
    };
    auto shared = std::make_shared<Shared>();
    auto loop = [shared, count, &fn] {
        for (;;) {
            const std::size_t i = shared->next.fetch_add(1);
            if (i >= count || shared->failed.load()) break;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (!shared->error) shared->error = std::current_exception();
                shared->failed = true;
            }
        }
    };
    const int helpers = int(std::min<std::size_t>(m_workers.size(), count - 1));
    shared->active = helpers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int h = 0; h < helpers; ++h) {
            m_tasks.push_back({shared.get(), [shared, loop] {
                loop();
                std::lock_guard<std::mutex> guard(shared->mutex);
                if (--shared->active == 0) shared->done.notify_all();
            }});
        }
    }
    m_wake.notify_all();
    loop();
    // Drop helpers that never started (all work is claimed), so nested or
    // concurrent calls cannot wait on tasks queued behind busy workers. Helpers
    // already running hold `fn` by reference, so wait for those.
    int dropped = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            if (it->owner == shared.get()) { it = m_tasks.erase(it); ++dropped; }
            else ++it;
        }
    }
    std::unique_lock<std::mutex> lock(shared->mutex);
    shared->active -= dropped;
    shared->done.wait(lock, [&] { return shared->active == 0; });
    if (shared->error) std::rethrow_exception(shared->error);
}

} // namespace vyra
